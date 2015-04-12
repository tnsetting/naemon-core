#include "naemon/events.h"
#include "naemon/query-handler.h"
#include "naemon/globals.h"
#include "naemon/workers.h"
#include "lib/libnaemon.h"
#include "worker.h"

/*
 * A note about worker tests:
 * One requirement for us to be able to read the output of the
 * job is that it fflush()'es its output buffers before it exits.
 * The exit() function does that, by properly flushing and closing
 * all open filedescriptors, but the _exit() function does not.
 * fflush() means that the kernel will copy the buffer from the
 * process' output buffer to the connected pipe's input buffer,
 * which is the "real" requirement for us to be able to read them
 * (and for poll() and epoll() to be able to mark them as readable).
 *
 * That means that some of these tests may look a bit weird, but
 * that's because the output buffers of a program belong to the
 * process and are destroyed in the instant the kernel reclaims
 * the process (ie, as part of making it reapable).
 *
 * There is no way for us to read data that isn't flushed. There
 * never will be a way for us to read data that isn't flushed,
 * and we can't *ever* do anything at all about it.
 * The tests cover such things as correct error codes during
 * timeouts, wait_status for signals and most cases I could
 * think of when I wrote them though.
 */
static struct wrk_test {
	char *command;
	char *expected_stdout;
	char *expected_stderr;

	char *full_command;
	int expected_wait_status;
	int expected_error_code;
	int timeout;
} test_jobs[] = {
	{
		"print=\"hello world\n\" exit=0",
		"hello world\n",
		"",
		NULL, 0, 0, 3
	},
	{
		"eprint='this goes to stderr\n' exit=0",
		"",
		"this goes to stderr\n",
		NULL, 0, 0, 3
	},
	{
		"print='natt' sleep=3 print='hatt' sleep=3 print='kattegatt' exit=2",
		"natthattkattegatt",
		"",
		NULL, 2 << 8, 0, 7
	},
	{
		"print='hoopla\n' fflush=1 _exit=0",
		"hoopla\n",
		"",
		NULL, 0, 0, 3
	},
	{
		"print=nocrlf close=1 fflush=1 _exit=0",
		"",
		"",
		NULL, 0, 0, 3
	},
	{
		"print=nocrlf eprint=lalala fflush=1 close=1 fflush=2 close=2 _exit=0",
		"nocrlf",
		"lalala",
		NULL, 0, 0, 3
	},
	{
		"sleep=50",
		"",
		"",
		NULL, 0, ETIME, 3,
	},
	{
		"print='lalala' fflush=1 sleep=50",
		"lalala",
		"",
		NULL, 0, ETIME, 3,
	},
	{
		"print=foo fflush=1 signal=6 sleep=20",
		"foo",
		"",
		NULL, 134, 0, 3,
	},
};
static int wrk_test_reaped;
static int wrk_test_failed;

void wrk_test_sighandler(int signo)
{
	printf("Caught signal %d. Aborting\n", signo);
	_exit(1);
}

void wrk_test_cb(struct wproc_result *wpres, void *data, int flags)
{
	struct wrk_test *t = (struct wrk_test *)data;
	int bad = 0;

	wrk_test_reaped++;
	printf("Reaping job: %s\n", t->command);
	if (wpres->wait_status != t->expected_wait_status) {
		printf("  wait_status %d != %d\n", wpres->wait_status, t->expected_wait_status);
		bad = 1;
	}
	if (wpres->error_code != t->expected_error_code) {
		printf("  error_code %d != %d\n", wpres->error_code, t->expected_error_code);
		bad = 1;
	}
	if (strcmp(wpres->outstd, t->expected_stdout)) {
		printf("###STDOUT###\n%s###STDOUTEND###\n", wpres->outstd);
		bad = 1;
	}
	if (strcmp(wpres->outerr, t->expected_stderr)) {
		printf("###STDERR###\n%s###STDERREND###\n", wpres->outerr);
		bad = 1;
	}
	wrk_test_failed += bad;

	free(t->full_command);
}

static int wrk_test_run(void)
{
	int result;
	unsigned int i;
	time_t max_timeout = 0;
	int num_tests = sizeof(test_jobs) / sizeof(test_jobs[0]);

	timing_point("Starting worker tests\n");
	signal(SIGCHLD, wrk_test_sighandler);
	for (i = 0; i < sizeof(test_jobs) / sizeof(test_jobs[0]); i++) {
		struct wrk_test *j = &test_jobs[i];
		result = asprintf(&j->full_command, "%s %s", naemon_binary_path, j->command);
		if (result < 0) {
			printf("Failed to create command line for job. Aborting\n");
			exit(1);
		}
		if (j->timeout > max_timeout)
			max_timeout = j->timeout;
		result = wproc_run_callback(j->full_command, j->timeout, wrk_test_cb, j, NULL);
		if (result) {
			printf("Failed to spawn job %d. Aborting\n", i);
			exit(1);
		}
	}

	do {
		iobroker_poll(nagios_iobs, max_timeout * 1000);
	} while (wrk_test_reaped < num_tests);

	timing_point("Exiting normally\n");
	if (wrk_test_failed)
		return 1;
	return 0;
}

/*
 * This is a really stupid "plugin" with scriptable behaviour.
 * It accepts commands and executes them in-order, like so:
 * usleep=<int> : usleep()'s the given number of microseconds
 * sleep=<int>  : sleep()'s the given number of seconds
 * print=<str>  : prints the given string to stdout
 * eprint=<str> : prints the given string to stderr
 * fflush=<1|2> : fflush()'es <stdout|stderr>
 * close=<int>  : closes the given filedescriptor
 * exit=<int>   : exit()'s with the given code
 * _exit=<int>  : _exit()'s with the given code
 * signal=<int> : sends the given signal to itself
 * Commands that aren't understood are simply printed as-is.
 */
static int wrk_test_plugin(int argc, char **argv)
{
	int i;

	/*
	 * i = 0 is not a typo here. We only get called with leftover args
	 * from the main program invocation
	 */
	for (i = 0; i < argc; i++) {
		char *eq, *cmd;
		int value;

		cmd = argv[i];

		if (!(eq = strchr(cmd, '='))) {
			printf("%s", argv[i]);
			continue;
		}

		*eq = 0;
		value = atoi(eq + 1);
		if (!strcmp(cmd, "usleep")) {
			usleep(value);
		}
		else if (!strcmp(cmd, "sleep")) {
			sleep(value);
		}
		else if (!strcmp(cmd, "print")) {
			printf("%s", eq + 1);
		}
		else if (!strcmp(cmd, "eprint")) {
			fprintf(stderr, "%s", eq + 1);
		}
		else if (!strcmp(cmd, "close")) {
			close(value);
		}
		else if (!strcmp(cmd, "exit")) {
			exit(value);
		}
		else if (!strcmp(cmd, "_exit")) {
			_exit(value);
		}
		else if (!strcmp(cmd, "signal")) {
			kill(getpid(), value);
		}
		else if (!strcmp(cmd, "fflush")) {
			if (value == 1)
				fflush(stdout);
			else if (value == 2)
				fflush(stderr);
			else
				fflush(NULL);
		}
	}

	return 0;
}

int main(int argc, char **argv)
{
	time_t start;

	enable_timing_point = 1;
	naemon_binary_path = argv[0];

	if (argc > 1) {
		if (!strcmp(argv[1], "--worker")) {
			return nm_core_worker(argv[2]);
		}
		return wrk_test_plugin(argc - 1, &argv[1]);
	}

	log_file = "/dev/stdout";

	timing_point("Initializing iobroker\n");
	if (!(nagios_iobs = iobroker_create())) {
		return EXIT_FAILURE;
	}

	timing_point("Initializing query handler socket\n");
	qh_socket_path = "/tmp/qh-socket";
	qh_init(qh_socket_path);


	timing_point("Launching workers\n");
	init_workers(3);
	start = time(NULL);
	while (wproc_num_workers_online < wproc_num_workers_spawned && time(NULL) < start + 45) {
		iobroker_poll(nagios_iobs, 1000);
	}
	timing_point("%d/%d workers online\n", wproc_num_workers_online, wproc_num_workers_spawned);

	if (wproc_num_workers_online != wproc_num_workers_spawned) {
		printf("Some workers failed to connect. Aborting\n");
		return EXIT_FAILURE;
	}

	return wrk_test_run();
}

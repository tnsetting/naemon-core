#ifndef _CHECKS_H
#define _CHECKS_H

#if !defined (_NAEMON_H_INSIDE) && !defined (NAEMON_COMPILATION)
#error "Only <naemon/naemon.h> can be included directly."
#endif

#include "objects.h"

/************ SERVICE DEPENDENCY VALUES ***************/

#define DEPENDENCIES_OK			0
#define DEPENDENCIES_FAILED		1

/***************** OBJECT CHECK TYPES *****************/
#define SERVICE_CHECK                   0
#define HOST_CHECK                      1

/* useful for hosts and services to determine time 'til next check */
#define normal_check_window(o) ((time_t)(o->check_interval * interval_length))
#define retry_check_window(o) ((time_t)(o->retry_interval * interval_length))
#define next_check_time(o) _next_check_time(o->last_check, check_window(o))
#define check_window(o) \
	((!o->current_state && o->state_type == SOFT_STATE) ? \
		retry_check_window(o) : \
		normal_check_window(o))

NAGIOS_BEGIN_DECL

static inline int _next_check_time(time_t last_check, time_t window)
{
	time_t now = time(NULL);

	/* if *_interval is 0, we mustn't schedule things immediately */
	if (!window)
		window = interval_length;

	/* last_check may be 0 or very far back if we've been shut down */
	if (last_check < (now - window))
		last_check = now;

	return last_check + window;
}

/*********************** GENERIC **********************/
void checks_init(void); /* Init check execution, schedule events */

int parse_check_output(char *, char **, char **, char **, int, int);
struct check_output *parse_output(const char *, struct check_output *);

NAGIOS_END_DECL

#endif

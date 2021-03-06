#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "load.h"
#include "sched_trace.h"
#include "eheap.h"


/* limit search window in case of missing completions */
#define MAX_COMPLETIONS_TO_CHECK 20
#define MAX_LOG 128


int want_ms = 0;
int total_jobs = 0;
int unsched = 0;
int log_preemptions[MAX_LOG];
int log_migrations[MAX_LOG];
int max_preemption = 0;
int max_migration = 0;
int statistic = 0;
int mute = 0;
int schedulability = 0;
int reserved = 0;

static double nano_to_ms(int64_t ns)
{
	return ns * 1E-6;
}

static void count_preemptions(
	struct evlink *release,
	struct evlink *completion,
	unsigned int *preemptions,
	unsigned int *migrations)
{
	struct evlink *e;
	int seen_switched_away = 0;
	u8 last_cpu = 0;

	for (e = release; e != completion; e = e->next) {
		if (e->rec->hdr.job == release->rec->hdr.job) {
			switch (e->rec->hdr.type) {
				case ST_SWITCH_AWAY:
					seen_switched_away = 1;
					last_cpu = e->rec->hdr.cpu;
					break;
				case ST_SWITCH_TO:
					if (seen_switched_away)
			    			(*preemptions)++;
			    		if (seen_switched_away &&
			    			e->rec->hdr.cpu != last_cpu)
			    			(*migrations)++;
			    		break;
			    	default:
			    		break;
			}
		}
	}
}

static void print_stats(
	struct task* t,
	struct evlink *release,
	struct evlink *completion)
{
	int64_t lateness;
	u64 response;
	u64 relative_deadline;
	unsigned int preemptions = 0, migrations = 0;

	lateness  = completion->rec->data.completion.when;
	lateness -= release->rec->data.release.deadline;
	response  = completion->rec->data.completion.when;
	response -= release->rec->data.release.release;
	relative_deadline = release->rec->data.release.deadline;
	relative_deadline -= release->rec->data.release.release;
	total_jobs++;
	if (lateness > 0)
		unsched++;

	count_preemptions(release, completion, &preemptions, &migrations);

  if (preemptions > max_preemption) 
		max_preemption = preemptions;
	log_preemptions[preemptions]++;


  if (migrations > max_migration) 
		max_migration = migrations;
	log_migrations[migrations]++;

	if (reserved) {
		//  schedulable #preemptions #migrations
		// printf("%d, %d, %d\n", lateness > 0, preemptions, migrations);
		printf("%5u, %5u, %10.2f, %10.2f, %10.2f, %8d, %10.2f, %12u, %12u\n",
						release->rec->hdr.pid,
						release->rec->hdr.job,
						nano_to_ms(completion->rec->data.completion.exec_time),
						nano_to_ms(relative_deadline),
						nano_to_ms(response),
						lateness > 0,
						nano_to_ms(lateness),
						preemptions,
						migrations);
	}

	if (!mute) {
		if (want_ms)
			printf(" %5u, %5u, %10.2f, %10.2f, %8d, %10.2f, %10.2f, %7d"
				", %10.2f, %12u, %12u\n",
						release->rec->hdr.pid,
						release->rec->hdr.job,
						nano_to_ms(per(t)),
						nano_to_ms(response),
						lateness > 0,
						nano_to_ms(lateness),
						lateness > 0 ? nano_to_ms(lateness) : 0,
						completion->rec->data.completion.forced,
						nano_to_ms(completion->rec->data.completion.exec_time),
						preemptions,
						migrations);
		else
			printf(" %5u, %5u, %10llu, %10llu, %8d, %10lld, %10lld, %7d"
				", %10llu, %12u, %12u\n",
						release->rec->hdr.pid,
						release->rec->hdr.job,
						(unsigned long long) per(t),
						(unsigned long long) response,
						lateness > 0,
						(long long) lateness,
						lateness > 0 ? (long long) lateness : 0,
						completion->rec->data.completion.forced,
						(unsigned long long) completion->rec->data.completion.exec_time,
						preemptions,
						migrations);
	}
}

static void print_task_info(struct task *t)
{
	if (!mute) {
		if (want_ms)
			printf("# task NAME=%s PID=%d COST=%.2f PERIOD=%.2f CPU=%d\n",
						tsk_name(t),
						t->pid,
						nano_to_ms(exe(t)),
						nano_to_ms(per(t)),
						tsk_cpu(t));
		else
			printf("# task NAME=%s PID=%d COST=%lu PERIOD=%lu CPU=%d\n",
						tsk_name(t),
						t->pid,
						(unsigned long) exe(t),
						(unsigned long) per(t),
						tsk_cpu(t));
	}	
}

static void usage(const char *str)
{
	fprintf(stderr,
		"\n  USAGE\n"
		"\n"
		"    st_job_stats [opts] <file.st>+\n"
		"\n"
		"  OPTIONS\n"
		"     -r        	 		 -- skip jobs prior to task-system release\n"
		"     -m        	 		 -- output milliseconds (default: nanoseconds)\n"
		"     -p PID       		 -- show only data for the task with the given PID\n"
		"     -n NAME       		-- show only data for the task(s) with the given NAME\n"
		"     -t PERIOD     	  -- show only data for the task(s) with the given PERIOD\n"
		"     -s STATISTICS 		-- show statistics\n"
		"     -S SCHEDULABILITY -- show whether all tasks are schedulable (yes = 1)\n"
		"     -R RESERVED			  -- reserved for DIY\n"
		"\n\n"
		);
	if (str) {
		fprintf(stderr, "Aborted: %s\n", str);
		exit(1);
	} else {
		exit(0);
	}
}

#define OPTSTR "rmsSRp:n:t:h"

int main(int argc, char** argv)
{
	unsigned int count;
	struct heap *h;

	struct task *t;
	struct evlink *e, *pos;
	struct st_event_record *rec;

	int wait_for_release = 0;
	u64 sys_release = 0;

	unsigned int pid_filter = 0;
	const char* name_filter = 0;
	u32 period_filter = 0;
	double sched_ratio = 0;

	int opt;

	memset(&log_preemptions, 0, sizeof(log_preemptions));
	memset(&log_migrations, 0, sizeof(log_migrations));

	while ((opt = getopt(argc, argv, OPTSTR)) != -1) {
		switch (opt) {
		case 'r':
			wait_for_release = 1;
			break;
		case 'm':
			want_ms = 1;
			break;
		case 's':
			mute = 1;
			statistic = 1;
			break;
		case 'S':
			mute = 1;
			schedulability = 1;
			break;
		case 'R':
			mute = 1;
			reserved = 1;
			break;
		case 'p':
			pid_filter = atoi(optarg);
			if (!pid_filter)
				usage("Invalid PID.");
			break;
		case 't':
			period_filter = atoi(optarg);
			if (!period_filter)
				usage("Invalid period.");
			break;
		case 'n':
			name_filter = optarg;
			break;
		case 'h':
			usage(NULL);
			break;
		case ':':
			usage("Argument missing.");
			break;
		case '?':
		default:
			usage("Bad argument.");
			break;
		}
	}

	if (want_ms)
		period_filter *= 1000000; /* ns per ms */

	h = load(argv + optind, argc - optind, &count);
	if (!h)
		return 1;

	init_tasks();
	split(h, count, 1);

	if (wait_for_release) {
		rec = find_sys_event(ST_SYS_RELEASE);
		if (rec)
			sys_release = rec->data.sys_release.release;
		else {
			fprintf(stderr, "Could not find task system "
				"release time.\n");
			exit(1);
		}
	}

	/* print header */
	if (!mute )
	printf("#%5s, %5s, %10s, %10s, %8s, %10s, %10s, %7s, %10s, %12s, %12s\n",
	       "Task",
	       "Job",
	       "Period",
	       "Response",
	       "DL Miss?",
	       "Lateness",
	       "Tardiness",
	       "Forced?",
	       "ACET",
	       "Preemptions",
	       "Migrations");

	/* print stats for each task */
	for_each_task(t) {
		if (pid_filter && pid_filter != t->pid)
			continue;
		if (name_filter && strcmp(tsk_name(t), name_filter))
			continue;
		if (period_filter && period_filter != per(t))
			continue;

		print_task_info(t);
		for_each_event(t, e) {
			rec = e->rec;
			if (rec->hdr.type == ST_RELEASE &&
			    (!wait_for_release ||
			     rec->data.release.release >= sys_release)) {
				pos  = e;
				count = 0;
				while (pos && count < MAX_COMPLETIONS_TO_CHECK) {
					find(pos, ST_COMPLETION);
					if (pos->rec->hdr.job == rec->hdr.job) {
						print_stats(t, e, pos);
						break;
					} else {
						pos = pos->next;
						count++;
					}
				}

			}
		}
	}

	if (statistic) {
		printf("Total number of jobs:%d\n", total_jobs);
		printf("Unsched:%d\n", unsched);
		sched_ratio = total_jobs - unsched;
		sched_ratio /= total_jobs;
		printf("Sched Ratio:%f\n", sched_ratio);

		for (int i = 0; i <= max_preemption; i++) {
			printf("Number of jobs that were preempted %d times: %d\n", i, log_preemptions[i]);
		}

		for (int i = 0; i <= max_migration; i++) {
			printf("Number of jobs that migrated %d times: %d\n", i, log_migrations[i]);
		}
	}

	if (schedulability) {
		printf("%d\n",(unsched==0)?1:0);
	}

	return 0;
}

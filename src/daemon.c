#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <strings.h>
#include <signal.h>
#include <string.h>
#include <fcntl.h>
#include <math.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/param.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#include <time.h>
#include <syslog.h>
#include <sys/time.h>
#include <netdb.h>
#include <pthread.h>
#include <inttypes.h>

#include "common.h"
#include "debug.h"
#if HAVE_LIBPCAP
#include "fg_pcap.h"
#endif
#include "fg_socket.h"
#include "fg_time.h"
#include "log.h"
#include "acl.h"
#include "daemon.h"
#include "source.h"
#include "destination.h"

#ifdef HAVE_FLOAT_H
#include <float.h>
#endif

#ifdef __SOLARIS__
#define RANDOM_MAX		4294967295UL	/* 2**32-1 */
#elif __DARWIN__
#define RANDOM_MAX		LONG_MAX	/* Darwin */
#else
#define RANDOM_MAX		RAND_MAX	/* Linux, FreeBSD */
#endif

#define CONGESTION_LIMIT 10000

int daemon_pipe[2];

int next_flow_id = 0;

pthread_mutex_t mutex;
struct _request *requests = 0, *requests_last = 0;

fd_set rfds, wfds, efds;
int maxfd;

struct _report* reports = 0;
struct _report* reports_last = 0;
unsigned int pending_reports = 0;

struct _flow flows[MAX_FLOWS];
unsigned int num_flows = 0;

char started = 0;

void flow_error(struct _flow *flow, const char *fmt, ...)
{
	char str[1000];

	int n;
	va_list ap;

	va_start(ap, fmt);
	n = vsnprintf(str, 1000, fmt, ap);
	va_end(ap);
	str[sizeof(str) - 1] = 0;
	flow->error = malloc(strlen(str) + 1);
	strcpy(flow->error, str);
}

void request_error(struct _request *request, const char *fmt, ...)
{
	char str[1000];

	int n;
	va_list ap;

	va_start(ap, fmt);
	n = vsnprintf(str, 1000, fmt, ap);
	va_end(ap);
	str[sizeof(str) - 1] = 0;
	request->error = malloc(strlen(str) + 1);
	strcpy(request->error, str);
}

static int flow_in_delay(struct timeval *now, struct _flow *flow, int direction)
{
	return time_is_after(&flow->start_timestamp[direction], now);
}

static int flow_sending(struct timeval *now, struct _flow *flow, int direction)
{
	return !flow_in_delay(now, flow, direction) &&
		(flow->settings.duration[direction] < 0 ||
		 time_diff(&flow->stop_timestamp[direction], now) < 0.0);
}

static int flow_block_scheduled(struct timeval *now, struct _flow *flow)
{
	return !flow->settings.write_rate ||
		time_is_after(now, &flow->next_write_block_timestamp);
}

void uninit_flow(struct _flow *flow)
{
	if (flow->fd_reply != -1)
		close(flow->fd_reply);
	if (flow->fd != -1)
		close(flow->fd);
	if (flow->listenfd_reply != -1)
		close(flow->listenfd_reply);
	if (flow->listenfd_data != -1)
		close(flow->listenfd_data);
	free(flow->read_block);
	free(flow->write_block);
	free(flow->addr);
	free(flow->error);
}

void remove_flow(unsigned int i)
{
	for (unsigned int j = i; j < num_flows - 1; j++)
		flows[j] = flows[j + 1];
	num_flows--;
	if (!num_flows)
		started = 0;
}

static void prepare_wfds(struct timeval *now, struct _flow *flow, fd_set *wfds)
{
	int rc = 0;

	if (flow_in_delay(now, flow, WRITE)) {
		DEBUG_MSG(4, "flow %i not started yet (delayed)", flow->id);
		return;
	}

	if (flow_sending(now, flow, WRITE)) {
		assert(!flow->finished[WRITE]);
		if (flow_block_scheduled(now, flow)) {
			DEBUG_MSG(4, "adding sock of flow %d to wfds", flow->id);
			FD_SET(flow->fd, wfds);
		} else {
			DEBUG_MSG(4, "no block for flow %d scheduled yet", flow->id);
		}
	} else if (!flow->finished[WRITE]) {
		flow->finished[WRITE] = 1;
		if (flow->settings.shutdown) {
			DEBUG_MSG(4, "shutting down flow %d (WR)", flow->id);
			rc = shutdown(flow->fd, SHUT_WR);
			if (rc == -1) {
				error(ERR_WARNING, "shutdown() SHUT_WR failed: %s",
						strerror(errno));
			}
#if HAVE_LIBPCAP
			fg_pcap_dispatch();
#endif
		}
	}

	return;
}

static int prepare_rfds(struct timeval *now, struct _flow *flow, fd_set *rfds)
{
	int rc = 0;

	if (!flow_in_delay(now, flow, READ) && !flow_sending(now, flow, READ)) {
		if (!flow->finished[READ] && flow->settings.shutdown) {
			error(ERR_WARNING, "server flow %u missed to shutdown", flow->id);
			rc = shutdown(flow->fd, SHUT_RD);
			if (rc == -1) {
				error(ERR_WARNING, "shutdown SHUT_RD "
						"failed: %s", strerror(errno));
			}
			flow->finished[READ] = 1;
		}
	}

	if (flow->source_settings.late_connect && !flow->connect_called ) {
		DEBUG_MSG(1, "late connecting test socket "
				"for flow %d after %.3fs delay",
				flow->id, flow->settings.delay[WRITE]);
		rc = connect(flow->fd, flow->addr,
				flow->addr_len);
		if (rc == -1 && errno != EINPROGRESS) {
			flow_error(flow, "Connect failed: %s", strerror(errno));
			return -1;
		}
		flow->connect_called = 1;
		flow->mtu = get_mtu(flow->fd);
		flow->mss = get_mss(flow->fd);
	}

	/* Altough the server flow might be finished we keep the socket in
	 * rfd in order to check for buggy servers */
	if (flow->connect_called && !flow->finished[READ]) {
		DEBUG_MSG(4, "adding sock of flow %d to rfds", flow->id);
		FD_SET(flow->fd, rfds);
	}

	return 0;
}

#ifdef __LINUX__
int get_tcp_info(struct _flow *flow, struct tcp_info *info);
#endif
static void report_flow(struct _flow* flow, int type);

static int prepare_fds() {

	unsigned int i = 0;

	FD_ZERO(&rfds);
	FD_ZERO(&wfds);
	FD_ZERO(&efds);

	FD_SET(daemon_pipe[0], &rfds);
	maxfd = daemon_pipe[0];

	struct timeval now;
	tsc_gettimeofday(&now);

	while (i < num_flows) {
		struct _flow *flow = &flows[i++];

		if (started && 
			(flow->finished[READ] || !flow->settings.duration[READ] || (!flow_in_delay(&now, flow, READ) && !flow_sending(&now, flow, READ))) &&
			(flow->finished[WRITE] || !flow->settings.duration[WRITE] || (!flow_in_delay(&now, flow, WRITE) && !flow_sending(&now, flow, WRITE)))) {

			/* Nothing left to read, nothing left to send */
			if (flow->fd != -1) {
#ifdef __LINUX__
				flow->statistics[TOTAL].has_tcp_info = get_tcp_info(flow, &flow->statistics[TOTAL].tcp_info) ? 0 : 1;
#endif
				flow->mtu = get_mtu(flow->fd);
				flow->mss = get_mss(flow->fd);

				report_flow(flow, TOTAL);
			}

			uninit_flow(flow);
			remove_flow(--i);
			continue;
		}

		if (flow->state == WAIT_ACCEPT_REPLY && flow->listenfd_reply != -1) {
			FD_SET(flow->listenfd_reply, &rfds);
			maxfd = MAX(maxfd, flow->listenfd_reply);
		}

		if (flow->state == GRIND_WAIT_ACCEPT && flow->listenfd_data != -1) {
			FD_SET(flow->listenfd_data, &rfds);
			maxfd = MAX(maxfd, flow->listenfd_data);
		}

		if (!started)
			continue;

		if (flow->fd_reply != -1) {
			FD_SET(flow->fd_reply, &rfds);
			maxfd = MAX(maxfd, flow->fd_reply);
		}

		if (flow->fd != -1) {
			FD_SET(flow->fd, &efds);
			maxfd = MAX(maxfd, flow->fd);

			prepare_wfds(&now, flow, &wfds);
			prepare_rfds(&now, flow, &rfds);
		}
	}

	return num_flows;
}

static void start_flows(struct _request_start_flows *request)
{
	struct timeval start;
	tsc_gettimeofday(&start);

#if 0
	if (start.tv_sec < request->start_timestamp) {
		/* If the clock is synchronized between nodes, all nodes will start 
		   at the same time regardless of any RPC delays */
		start.tv_sec = request->start_timestamp;
		start.tv_usec = 0;
	}
#else
	UNUSED_ARGUMENT(request);
#endif

	for (unsigned int i = 0; i < num_flows; i++) {
		struct _flow *flow = &flows[i];

		/* READ and WRITE */
		for (int j = 0; j < 2; j++) {
			flow->start_timestamp[j] = start;
			time_add(&flow->start_timestamp[j], flow->settings.delay[j]);
			if (flow->settings.duration[j] >= 0) {
				flow->stop_timestamp[j] = flow->start_timestamp[j];
				time_add(&flow->stop_timestamp[j], flow->settings.duration[j]);
			}
		}
		if (flow->settings.write_rate)
			flow->next_write_block_timestamp = flow->start_timestamp[WRITE];

		tsc_gettimeofday(&flow->last_report_time);
		flow->first_report_time = flow->last_report_time;
		flow->next_report_time = flow->last_report_time;

		time_add(&flow->next_report_time, flow->settings.reporting_interval);
	}

	started = 1;
}

static void stop_flow(struct _request_stop_flow *request)
{
	if (request->flow_id == -1) {
		/* Stop all flows */

		for (unsigned int i = 0; i < num_flows; i++) {
			struct _flow *flow = &flows[i];

			uninit_flow(flow);
			remove_flow(i);
		}

		return;
	}

	for (unsigned int i = 0; i < num_flows; i++) {
		struct _flow *flow = &flows[i];

		if (flow->id != request->flow_id)
			continue;

		uninit_flow(flow);
		remove_flow(i);
		return;
	}

	request_error(&request->r, "Unknown flow id");
}

static void process_requests()
{
	int rc;
	pthread_mutex_lock(&mutex);

	char tmp[100];
	for (;;) {
		int rc = read(daemon_pipe[0], tmp, 100);
		if (rc != 100)
			break;
	}

	while (requests)
	{
		struct _request* request = requests;
		requests = requests->next;
		rc = 0;

		switch (request->type) {
		case REQUEST_ADD_DESTINATION:
			add_flow_destination((struct _request_add_flow_destination *)request);
			break;
		case REQUEST_ADD_SOURCE:
			rc = add_flow_source((struct _request_add_flow_source *)request);
			break;
		case REQUEST_START_FLOWS:
			start_flows((struct _request_start_flows *)request);
			break;
		case REQUEST_STOP_FLOW:
			stop_flow((struct _request_stop_flow *)request);
			break;
		case REQUEST_GET_STATUS:
			{
				struct _request_get_status *r = (struct _request_get_status *)request;
				r->started = started;
				r->num_flows = num_flows;
			}
			break;
		default:
			request_error(request, "Unknown request type");
			break;
		}
		if (rc != 1)
			pthread_cond_signal(request->condition);
	};

	pthread_mutex_unlock(&mutex);
}

/*
 * Prepare a report.
 * type is either INTERVAL or TOTAL
 */
static void report_flow(struct _flow* flow, int type)
{
	struct _report* report = (struct _report*)malloc(sizeof(struct _report));
	
	report->id = flow->id;
	report->type = type;

	if (type == INTERVAL)
		report->begin = flow->last_report_time;
	else
		report->begin = flow->first_report_time;

	tsc_gettimeofday(&report->end);
	flow->last_report_time = report->end;
	report->bytes_read = flow->statistics[type].bytes_read;
	report->bytes_written = flow->statistics[type].bytes_written;
	report->reply_blocks_read = flow->statistics[type].reply_blocks_read;

	report->rtt_min = flow->statistics[type].rtt_min;
	report->rtt_max = flow->statistics[type].rtt_max;
	report->rtt_sum = flow->statistics[type].rtt_sum;
	report->iat_min = flow->statistics[type].iat_min;
	report->iat_max = flow->statistics[type].iat_max;
	report->iat_sum = flow->statistics[type].iat_sum;

#ifdef __LINUX__
	if (flow->statistics[type].has_tcp_info)
		report->tcp_info = flow->statistics[type].tcp_info;
	else
		memset(&report->tcp_info, 0, sizeof(struct tcp_info));
#endif
	if (flow->fd != -1) {
		/* Get latest MTU and MSS */
		int mtu, mss;
		mtu = get_mtu(flow->fd);
		mss = get_mss(flow->fd);
		if (mtu != -1)
			flow->mtu = mtu;
		if (mss != -1)
			flow->mss = mss;
	}
	report->mss = flow->mss;
	report->mtu = flow->mtu;

	/* Add status flags to report */
	report->status = 0;

	if (flow->statistics[type].bytes_read == 0) {
		if (flow_in_delay(&report->end, flow, READ))
			report->status |= 'd';
		else if (flow_sending(&report->end, flow, READ))
			report->status |= 'l';
		else if (flow->settings.duration[READ] == 0)
			report->status |= 'o';
		else
			report->status |= 'f';
	} else {
		if (!flow_sending(&report->end, flow, READ) && !flow->finished)
			report->status |= 'c';
		else
			report->status |= 'n';
	}
	report->status <<= 8;
	
	if (flow->statistics[type].bytes_written < flow->settings.write_block_size) {
		if (flow_in_delay(&report->end, flow, WRITE))
			report->status |= 'd';
		else if (flow_sending(&report->end, flow, WRITE))
			report->status |= 'l';
		else if (flow->settings.duration[WRITE] == 0)
			report->status |= 'o';
		else
			report->status |= 'f';
	} else {
		if (!flow_sending(&report->end, flow, WRITE) && !flow->finished)
			report->status |= 'c';
		else
			report->status |= 'n';
	}
	

	/* New report interval, reset old data */
	if (type == INTERVAL) {
		flow->statistics[INTERVAL].bytes_read = 0;
		flow->statistics[INTERVAL].bytes_written = 0;
		flow->statistics[INTERVAL].reply_blocks_read = 0;

		flow->statistics[INTERVAL].rtt_min = +INFINITY;
		flow->statistics[INTERVAL].rtt_max = -INFINITY;
		flow->statistics[INTERVAL].rtt_sum = 0;
		flow->statistics[INTERVAL].iat_min = +INFINITY;
		flow->statistics[INTERVAL].iat_max = -INFINITY;
		flow->statistics[INTERVAL].iat_sum = 0;
	}

	add_report(report);
}

#ifdef __LINUX__
int get_tcp_info(struct _flow *flow, struct tcp_info *info)
{
	struct tcp_info tmp_info;
	socklen_t info_len = sizeof(tmp_info);
	int rc;

	rc = getsockopt(flow->fd, IPPROTO_TCP, TCP_INFO, &tmp_info, &info_len);
	if (rc == -1) {
		error(ERR_WARNING, "getsockopt() failed: %s",
				strerror(errno));
		return -1;
	}
	*info = tmp_info;

	return 0;
}
#endif

static void timer_check()
{
	struct timeval now;

	if (!started)
		return;

	tsc_gettimeofday(&now);
	for (unsigned int i = 0; i < num_flows; i++) {
		struct _flow *flow = &flows[i];

		if (!flow->settings.reporting_interval)
			continue;

		if (!time_is_after(&now, &flow->next_report_time))
			continue;

#ifdef __LINUX__
		if (flow->fd != -1)
			flow->statistics[INTERVAL].has_tcp_info = get_tcp_info(flow, &flow->statistics[INTERVAL].tcp_info) ? 0 : 1;
#endif
		report_flow(flow, INTERVAL);

		do {
			time_add(&flow->next_report_time, flow->settings.reporting_interval);
		} while (time_is_after(&now, &flow->next_report_time));
	}
}

static int write_data(struct _flow *flow);
static int read_data(struct _flow *flow);
static int read_reply(struct _flow *flow);

static void process_select(fd_set *rfds, fd_set *wfds, fd_set *efds)
{
	unsigned int i = 0;
	while (i < num_flows) {
		struct _flow *flow = &flows[i];

		/* If any function fails, the flow has ended. */
		if (flow->listenfd_reply != -1 && FD_ISSET(flow->listenfd_reply, rfds)) {
			if (flow->state == WAIT_ACCEPT_REPLY) {
				if (accept_reply(flow) == -1)
					goto remove;
			}
		}
		if (flow->listenfd_data != -1 && FD_ISSET(flow->listenfd_data, rfds)) {
			if (flow->state == GRIND_WAIT_ACCEPT) {
				if (accept_data(flow) == -1)
					goto remove;
			}
		}

		if (flow->fd_reply != -1 && FD_ISSET(flow->fd_reply, rfds))
			if (read_reply(flow) == -1)
				goto remove;

		if (flow->fd != -1) {

			if (FD_ISSET(flow->fd, efds)) {
				int error_number, rc;
				socklen_t error_number_size = sizeof(error_number);
				DEBUG_MSG(5, "sock of flow %d in efds", flow->id);
				rc = getsockopt(flow->fd, SOL_SOCKET,
						SO_ERROR,
						(void *)&error_number,
						&error_number_size);
				if (rc == -1) {
					error(ERR_WARNING, "failed to get "
							"errno for non-blocking "
							"connect: %s",
							strerror(errno));
					goto remove;
				}
				if (error_number != 0) {
					fprintf(stderr, "connect: %s\n",
							strerror(error_number));
					goto remove;
				}
			}
			if (FD_ISSET(flow->fd, wfds))
				if (write_data(flow) == -1)
					goto remove;
			if (FD_ISSET(flow->fd, rfds))
				if (read_data(flow) == -1)
					goto remove;
		}

		i++;
		continue;
remove:
		// Flow has ended
#ifdef __LINUX__
		if (flow->fd != -1) {
			flow->statistics[TOTAL].has_tcp_info = get_tcp_info(flow, &flow->statistics[TOTAL].tcp_info) ? 0 : 1;
			flow->mtu = get_mtu(flow->fd);
			flow->mss = get_mss(flow->fd);

			report_flow(flow, TOTAL);
		}
#endif
		uninit_flow(flow);
		remove_flow(i);
	}
}

void* daemon_main(void* ptr __attribute__((unused)))
{
	struct timeval timeout;
	for (;;) {
		int need_timeout = prepare_fds();

		timeout.tv_sec = 0;
		timeout.tv_usec = 10000;

		int rc = select(maxfd + 1, &rfds, &wfds, &efds, need_timeout ? &timeout : 0);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			error(ERR_FATAL, "select() failed: %s",
					strerror(errno));
			exit(1);
		}

		if (FD_ISSET(daemon_pipe[0], &rfds))
			process_requests();

		timer_check();

		process_select(&rfds, &wfds, &efds);
	}
}

void add_report(struct _report* report)
{
	pthread_mutex_lock(&mutex);

	// Do not keep too much data
	if (pending_reports >= 100 && report->type != TOTAL) {
		free(report);
		pthread_mutex_unlock(&mutex);
		return;
	}

	report->next = 0;

	if (reports_last)
		reports_last->next = report;
	else
		reports = report;
	reports_last = report;

	pending_reports++;

	pthread_mutex_unlock(&mutex);
}

struct _report* get_reports(int *has_more)
{
	const unsigned int max_reports = 50;

	struct _report* ret;

	pthread_mutex_lock(&mutex);

	ret = reports;

	if (pending_reports <= max_reports) {
		*has_more = 0;
		pending_reports = 0;
		reports = NULL;
		reports_last = 0;
	}
	else {
		/* Split off first 50 items */
		struct _report* tmp;
		for (unsigned int i = 0; i < max_reports - 1; i++)
			reports = reports->next;
		tmp = reports->next;
		reports->next = 0;
		reports = tmp;

		pending_reports -= max_reports;
		*has_more = 1;
	}

	pthread_mutex_unlock(&mutex);

	return ret;
}

void init_flow(struct _flow* flow, int is_source)
{
	flow->id = next_flow_id++;
	flow->state = is_source ? WAIT_CONNECT_REPLY : WAIT_ACCEPT_REPLY;
	flow->fd = -1;
	flow->fd_reply = -1;
	flow->listenfd_reply = -1;
	flow->listenfd_data = -1;

	flow->read_block = 0;
	flow->read_block_bytes_read = 0;
	flow->read_block_count = 0;
	flow->write_block = 0;
	flow->write_block_bytes_written = 0;
	flow->write_block_count = 0;

	flow->reply_block_bytes_read = 0;

	flow->last_block_read.tv_sec = 0;
	flow->last_block_read.tv_usec = 0;

	flow->connect_called = 0;
	flow->finished[READ] = flow->finished[WRITE] = 0;

	flow->addr = 0;

	/* INTERVAL and TOTAL */
	for (int i = 0; i < 2; i++) {
		flow->statistics[i].bytes_read = 0;
		flow->statistics[i].bytes_written = 0;
		flow->statistics[i].reply_blocks_read = 0;

		flow->statistics[i].rtt_min = +INFINITY;
		flow->statistics[i].rtt_max = -INFINITY;
		flow->statistics[i].rtt_sum = 0;

		flow->statistics[i].iat_min = +INFINITY;
		flow->statistics[i].iat_max = -INFINITY;
		flow->statistics[i].iat_sum = 0;
	}

	flow->congestion_counter = 0;

	flow->error = 0;
}

static double flow_interpacket_delay(struct _flow *flow)
{
	double delay = 0;

	DEBUG_MSG(5, "flow %d has rate %u", flow->id, flow->settings.write_rate);
	if (flow->settings.poisson_distributed) {
		double urand = (double)((random()+1.0)/(RANDOM_MAX+1.0));
		double erand = -log(urand) * 1/(double)flow->settings.write_rate;
		delay = erand;
	} else {
		delay = (double)1/flow->settings.write_rate;
	}

	DEBUG_MSG(5, "new interpacket delay %.6f for flow %d.", delay, flow->id);
	return delay;
}

static int write_data(struct _flow *flow)
{
	int rc = 0;

	/* Please note: you could argue that the following loop
	   is not necessary as not filling the socket send queue completely
	   would make the next select call return this very socket in wfds
	   and thus sending more blocks would immediately happen. However,
	   calling select with a non-full send queue might make the kernel
	   think we don't have more data to send. As a result, the kernel
	   might trigger some scheduling or whatever heuristics which would
	   not take place if we had written immediately. On the other hand,
	   in case the network is not a bottleneck the loop may take forever. */
	/* XXX: Detect this! */
	for (;;) {
		if (flow->write_block_bytes_written == 0) {
			DEBUG_MSG(5, "new write block %llu on flow %d",
					(long long unsigned int)flow->write_block_count, flow->id);
			flow->write_block[0] = sizeof(struct timeval) + 1;
			tsc_gettimeofday((struct timeval *)(flow->write_block + 1));
		}

		rc = write(flow->fd,
				flow->write_block +
				flow->write_block_bytes_written,
				flow->settings.write_block_size -
				flow->write_block_bytes_written);

		if (rc == -1) {
			if (errno == EAGAIN) {
				DEBUG_MSG(5, "write queue limit hit "
						"for flow %d", flow->id);
				break;
			}
			flow_error(flow, "Premature end of test: %s",
					strerror(errno));
			return -1;
		}

		if (rc == 0) {
			DEBUG_MSG(5, "flow %d sent zero bytes. what does that mean?", flow->id);
			break;
		}

		DEBUG_MSG(4, "flow %d sent %d bytes of %u (already = %u)", flow->id, rc,
				flow->settings.write_block_size,
				flow->write_block_bytes_written);

		flow->statistics[INTERVAL].bytes_written += rc;
		flow->statistics[TOTAL].bytes_written += rc;
		flow->write_block_bytes_written += rc;
		if (flow->write_block_bytes_written >=
				(unsigned int)flow->settings.write_block_size) {
			flow->write_block_bytes_written = 0;
			tsc_gettimeofday(&flow->last_block_written);
			flow->write_block_count++;

			if (flow->settings.write_rate) {
				time_add(&flow->next_write_block_timestamp,
						flow_interpacket_delay(flow));
				if (time_is_after(&flow->last_block_written, &flow->next_write_block_timestamp)) {
					/* TODO: log time_diff and check if
					 * it's growing (queue build up) */
					DEBUG_MSG(3, "incipient congestion on "
							"flow %u (block %" PRIu64 "): "
							"new block scheduled "
							"for %s, %.6lfs before now.",
							flow->id,
							flow->write_block_count,
							ctime_us(&flow->next_write_block_timestamp),
							time_diff(&flow->next_write_block_timestamp, &flow->last_block_written));
					flow->congestion_counter++;
					if (flow->congestion_counter >
							CONGESTION_LIMIT &&
							flow->settings.flow_control) {
						return -1;
					}
					
				}
			}
			if (flow->settings.cork && toggle_tcp_cork(flow->fd) == -1)
				DEBUG_MSG(4, "failed to recork test socket "
						"for flow %d: %s",
						flow->id, strerror(errno));
		}

		if (!flow->settings.pushy)
			break;
	}
	return 0;
}

static int read_data(struct _flow *flow)
{
	int rc;
	struct iovec iov;
	struct msghdr msg;
	char cbuf[512];
	struct cmsghdr *cmsg;

	for (;;) {
		if (flow->read_block_bytes_read == 0)
			DEBUG_MSG(5, "new read block %llu on flow %d",
					(long long unsigned int)flow->read_block_count, flow->id);

		iov.iov_base = flow->read_block +
			flow->read_block_bytes_read;
		iov.iov_len = flow->settings.read_block_size -
			flow->read_block_bytes_read;
		// no name required
		msg.msg_name = NULL;
		msg.msg_namelen = 0;
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_control = cbuf;
		msg.msg_controllen = sizeof(cbuf);
		rc = recvmsg(flow->fd, &msg, 0);

		if (rc == -1) {
			if (errno == EAGAIN)
				break;
			flow_error(flow, "Premature end of test: %s",
					strerror(errno));
			return -1;
		}

		if (rc == 0) {
			DEBUG_MSG(1, "server shut down test socket "
					"of flow %d", flow->id);
			if (!flow->finished[READ] ||
					!flow->settings.shutdown)
				error(ERR_WARNING, "Premature shutdown of "
						"server flow");
			flow->finished[READ] = 1;
			if (flow->finished[WRITE]) {
				DEBUG_MSG(4, "flow %u finished", flow->id);
				return -1;
			}
			return 0;
		}

		DEBUG_MSG(4, "flow %d received %u bytes", flow->id, rc);

#if 0
		if (flow->settings[DESTINATION].duration[WRITE] == 0)
			error(ERR_WARNING, "flow %d got unexpected data "
					"from server (no two-way)", id);
		else if (server_flow_in_delay(id))
			error(ERR_WARNING, "flow %d got unexpected data "
					"from server (too early)", id);
		else if (!server_flow_sending(id))
			error(ERR_WARNING, "flow %d got unexpected data "
					"from server (too late)", id);
#endif

		flow->statistics[INTERVAL].bytes_read += rc;
		flow->statistics[TOTAL].bytes_read += rc;
		flow->read_block_bytes_read += rc;
		if (flow->read_block_bytes_read >= (unsigned int)flow->settings.read_block_size) {
			assert(flow->read_block_bytes_read == (unsigned int)flow->settings.read_block_size);

			flow->read_block_count++;
			flow->read_block_bytes_read = 0;
			/* the size of the reply is stored in the first byte of the incoming block */
			/* this size is echoed back from the received block, to calculate RTT */
			int reply_block_length = flow->read_block[0] + sizeof(double);
			double *iat_ptr = (double *)(flow->read_block
				+ flow->read_block[0]);
			if (flow->settings.read_block_size >= reply_block_length) {
				if (flow->last_block_read.tv_sec == 0 &&
					flow->last_block_read.tv_usec == 0) {
					*iat_ptr = NAN;
					DEBUG_MSG(5, "isnan = %d",
						isnan(*iat_ptr));
				} else
					*iat_ptr = time_diff_now(
						&flow->last_block_read);
				tsc_gettimeofday(&flow->last_block_read);
				rc = write(flow->fd_reply, flow->read_block,
						reply_block_length);
				if (rc == -1) {
					if (errno == EAGAIN) {
						logging_log(LOG_WARNING,
							"congestion on "
							"control connection, "
							"dropping reply block");
					}
					else {
						logging_log(LOG_WARNING,
							"Premature end of test: %s",
							strerror(errno));
						return -1;
					}
				}
				else {
					DEBUG_MSG(4, "sent reply block (IAT = "
						"%.3lf)", (isnan(*iat_ptr) ?
							NAN : (*iat_ptr) * 1e3));
				}
			}
		}

		for (cmsg = CMSG_FIRSTHDR(&msg); cmsg;
				cmsg = CMSG_NXTHDR(&msg, cmsg)) {
			DEBUG_MSG(2, "flow %d received cmsg: type = %u, len = %zu",
					flow->id, cmsg->cmsg_type, cmsg->cmsg_len);
		}

		if (!flow->settings.pushy)
			break;
	}
	return 0;
}

static void process_reply(struct _flow* flow)
{
	struct timeval now;
	/* XXX: There is actually a conversion from
		network to host byte order needed here!! */
	struct timeval *sent = (struct timeval *)(flow->reply_block + 1);
	double current_rtt;
	double *current_iat_ptr = (double *)(flow->reply_block + sizeof(struct timeval) + 1);

	tsc_gettimeofday(&now);
	current_rtt = time_diff(sent, &now);

	if ((!isnan(*current_iat_ptr) && *current_iat_ptr <= 0) || current_rtt <= 0) {
		DEBUG_MSG(5, "illegal reply_block: isnan = %d, iat = %e, rtt = %e", isnan(*current_iat_ptr), *current_iat_ptr, current_rtt);
		error(ERR_WARNING, "Found block with illegal round trip time or illegal inter arrival time, ignoring block.");
		return;
	}

	/* Update statistics for flow, both INTERVAL and TOTAL. */
	for (int i = 0; i < 2; i++) {
		flow->statistics[i].reply_blocks_read++;

		/* Round trip times */
		ASSIGN_MIN(flow->statistics[i].rtt_min, current_rtt);
		ASSIGN_MAX(flow->statistics[i].rtt_max, current_rtt);
		flow->statistics[i].rtt_sum += current_rtt;
	
		/* Inter arrival times */
		if (!isnan(*current_iat_ptr)) {
			ASSIGN_MIN(flow->statistics[i].iat_min, *current_iat_ptr);
			ASSIGN_MAX(flow->statistics[i].iat_max, *current_iat_ptr);
			flow->statistics[i].iat_sum += *current_iat_ptr;
		}

	}
	
	DEBUG_MSG(4, "processed reply_block of flow %d, (RTT = %.3lfms, IAT = %.3lfms)", flow->id, current_rtt * 1e3, isnan(*current_iat_ptr) ? NAN : *current_iat_ptr * 1e3);
}

static int read_reply(struct _flow *flow)
{
	int rc = 0;

	for (;;) {
		rc = recv(flow->fd_reply,
				flow->reply_block + flow->reply_block_bytes_read,
				sizeof(flow->reply_block) -
				flow->reply_block_bytes_read, 0);
		if (rc == -1) {
			if (errno == EAGAIN)
				break;
			flow_error(flow, "Premature end of test: %s",
					strerror(errno));
			return -1;
		}

		if (rc == 0) {
			error(ERR_WARNING, "Premature end of test: server "
					"shut down reply connection of flow %d.", flow->id);
			return -1;
		}

		flow->reply_block_bytes_read += rc;
		if (flow->reply_block_bytes_read >=
				sizeof(flow->reply_block)) {
			process_reply(flow);
			flow->reply_block_bytes_read = 0;
		} else {
			DEBUG_MSG(4, "got partial reply_block for flow %d", flow->id);
		}

	}
	return 0;
}

int apply_extra_socket_options(struct _flow *flow)
{
	int i;

	for (i = 0; i < flow->settings.num_extra_socket_options; i++) {

		int level, res;
		const struct _extra_socket_options *option = &flow->settings.extra_socket_options[i];

		switch (option->level) {
			case level_sol_socket:
				level = SOL_SOCKET;
				break;
			case level_sol_tcp:
				level = SOL_TCP;
				break;
			case level_ipproto_ip:
				level = IPPROTO_IP;
				break;
			case level_ipproto_sctp:
				level = IPPROTO_SCTP;
				break;
			case level_ipproto_tcp:
				level = IPPROTO_TCP;
				break;
			case level_ipproto_udp:
				level = IPPROTO_UDP;
				break;
			default:
				flow_error(flow, "Unknown socket option level: %d", option->level);
				return -1;
		}

		res = setsockopt(flow->fd, level, option->optname, option->optval, option->optlen);

		if (res == -1) {
			flow_error(flow, "Unable to set socket option %d: %s", option->optname, strerror(errno));
			return -1;
		}
	}

	return 0;
}

/* Set the TCP options on the data socket */
int set_flow_tcp_options(struct _flow *flow)
{
	set_non_blocking(flow->fd);

	if (*flow->settings.cc_alg && set_congestion_control(
				flow->fd, flow->settings.cc_alg) == -1) {
		flow_error(flow, "Unable to set congestion control algorithm: %s",
				strerror(errno));
		return -1;
	}

	if (*flow->settings.ro_alg && set_reorder(
				flow->fd, flow->settings.ro_alg) == -1) {
		flow_error(flow, "Unable to set reorder algorithm: %s",
				strerror(errno));
		return -1;
	}

	if (flow->settings.elcn && set_so_elcn(flow->fd, flow->settings.elcn) == -1) {
		flow_error(flow, "Unable to set TCP_ELCN: %s", strerror(errno));
		return -1;
	}

	if (flow->settings.icmp && set_so_icmp(flow->fd) == -1) {
		flow_error(flow, "Unable to set TCP_ICMP: %s",
			strerror(errno));
		return -1;
	}

	if (flow->settings.cork && set_tcp_cork(flow->fd) == -1) {
		flow_error(flow, "Unable to set TCP_CORK: %s",
			strerror(errno));
		return -1;
	}

#if HAVE_LIBPCAP
	if (flow->settings.advstats)
		fg_pcap_go(flow->fd);
#endif

	if (flow->settings.so_debug && set_so_debug(flow->fd) == -1) {
		flow_error(flow, "Unable to set SO_DEBUG: %s",
			strerror(errno));
		return -1;
	}

	if (flow->settings.route_record && set_route_record(flow->fd) == -1) {
		flow_error(flow, "Unable to set route record option: %s",
			strerror(errno));
		return -1;
	}

	if (flow->settings.dscp && set_dscp(flow->fd, flow->settings.dscp) == -1) {
		flow_error(flow, "Unable to set DSCP value: %s",
			strerror(errno));
		return -1;
	}

	if (flow->settings.ipmtudiscover && set_ip_mtu_discover(flow->fd) == -1) {
		flow_error(flow, "Unable to set IP_MTU_DISCOVER value: %s",
			strerror(errno));
		return -1;
	}

	if (apply_extra_socket_options(flow) == -1)
		return -1;

	return 0;
}

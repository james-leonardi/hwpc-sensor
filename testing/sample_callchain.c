#include <stdlib.h>
#include <stdio.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <errno.h>

#define PAGE_SIZE 4096

typedef unsigned long u64;

struct read_format {
	u64 value;
	u64 time_enabled;
	u64 time_running;
	u64 id;
	u64 lost;
};

struct sample {
	struct perf_event_header header;
	u64 nr;
	u64 ips[];
};

void print_mmap_page(struct perf_event_mmap_page *header) {
	printf("struct perf_event_mmap_page\n");
	printf("\tversion: %u\n", header->version);
	printf("\tcompat_version: %u\n", header->compat_version);
	printf("\tlock: %u\n", header->lock);

	printf("\tindex: %u\n", header->index);
	printf("\toffset: %lli\n", header->offset);
	printf("\ttime_enabled: %llu\n", header->time_enabled);
	printf("\ttime_running: %llu\n", header->time_running);
	printf("\tcapabilities: %llu\n", header->capabilities);
	
	printf("\tpmc_width: %hu\n", header->pmc_width);
	printf("\ttime_shift: %hu\n", header->time_shift);
	printf("\ttime_mult: %u\n", header->time_mult);
	printf("\ttime_offset: %llu\n", header->time_offset);
	
	printf("\tdata_head: %llu\n", header->data_head);
	printf("\tdata_tail: %llu\n", header->data_tail);
	printf("\tdata_offset: %llu\n", header->data_offset);
	printf("\tdata_size: %llu\n", header->data_size);

	printf("\taux_head: %llu\n", header->aux_head);
	printf("\taux_tail: %llu\n", header->aux_tail);
	printf("\taux_offset: %llu\n", header->aux_offset);
	printf("\taux_size: %llu\n", header->aux_size);
	
	printf("\n");
}

void print_header(struct perf_event_header *header) {
	printf("struct perf_event_header\n");
	printf("\ttype: %u\n", header->type);
	printf("\tmisc: %hu\n", header->misc);
	printf("\tsize: %hu\n", header->size);

	printf("\n");
}

void print_sample(struct sample *sample) {
	print_header(&sample->header);
	printf("struct sample\n");
	printf("\tnr: %lu\n", sample->nr);
	for (int i = 0; i < sample->nr; i++)
		printf("\t\tips[%i]: %lu\n", i, sample->ips[i]);
	printf("\n\n");
}

int main(int argc, char** argv) {
	unsigned int samp_freq = 1000;
	unsigned int report_sleep = 1000000;
	if (argc >= 2) {
		samp_freq = atoi(argv[1]);
	}
	if (argc >= 3) {
		report_sleep = atoi(argv[2]);
	}

	// Create struct perf_event_attr
	struct perf_event_attr attr = {0};
	// Fields
	attr.size = sizeof(struct perf_event_attr);
	attr.type = PERF_TYPE_HARDWARE;
	attr.config = PERF_COUNT_HW_INSTRUCTIONS;
	attr.sample_type = PERF_SAMPLE_CALLCHAIN;
	attr.sample_freq = samp_freq;
	// Flags
	attr.mmap = 1;
	attr.freq = 1;
	attr.ksymbol = 1;
	// Require an enable call to start recording
	attr.disabled = 1;

	// Invoke the perf recorder
	int fd = syscall(SYS_perf_event_open, &attr, 0, -1, -1, 0);
	if (fd == -1) {
		perror("perf_event_open");
		exit(EXIT_FAILURE);
	}

	// Set up memory map to collect results
	void *buffer = mmap(NULL, 1+16*PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (buffer == MAP_FAILED) {
		perror("mmap");
		close(fd);
		exit(EXIT_FAILURE);
	}

	// Enable sampling
	ioctl(fd, PERF_EVENT_IOC_RESET, 0);
	ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);

	// Save a pointer to the info struct
	struct perf_event_mmap_page *buffer_info = buffer;

	// Continuously read samples and print using print_sample()
	void *data_head = buffer + PAGE_SIZE;
	struct perf_event_header *hdr;
	while (1) {
		print_mmap_page(buffer_info);
		print_sample(data_head);

		// Increment our read pointer by the amount of bytes read.
		hdr = data_head;
		data_head += hdr->size;
		// Increment data_tail by the amount of bytes read.
		buffer_info->data_tail += hdr->size;

		// Sleep for 1 sec
		usleep(report_sleep);

		// This does not account for wrapping yet...
	}
}

// For reading standard metrics (reported from fd)
/*
	// Set up buffer to read results
	struct read_format results[100] = {0};
	// Read 1 result into buffer for testing
	read(fd, results, sizeof(struct read_format));
	printf("Value: %lu\nTime Enabled:%lu\nTime Running: %lu\nID:%lu\n",
			results[0].value, results[0].time_enabled, results[0].time_running, results[0].id);
*/


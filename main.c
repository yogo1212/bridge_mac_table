#include <arpa/inet.h>
#include <errno.h>
#include <linux/if.h>
#include <linux/if_bridge.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <json-c/json.h>

// stolen from linux
#define BR_PORT_BITS    10
#define BR_MAX_PORTS    (1<<BR_PORT_BITS)

typedef struct {
	char n[BR_MAX_PORTS][IFNAMSIZ + 1];
} ifnames_by_port_t;

typedef void (*mac_table_entry_cb_t)(const char *mac, const char *ifname, uint32_t age, bool local, void *arg);

static void print_mac_table_entry(const char *mac, const char *ifname, uint32_t age, bool local, void *arg)
{
	char delim = *(char *) arg;
	printf("%s%c%s%c%d%c%d\n", mac, delim, ifname, delim, age, delim, !!local);
}

static void add_mac_table_entry_to_json(const char *mac, const char *ifname, uint32_t age, bool local, void *arg)
{
	struct json_object *jlist = (struct json_object *) arg;
	struct json_object *jobj;

	jobj = json_object_new_object();

	json_object_object_add(jobj, "ifname",
		json_object_new_string(ifname));

	json_object_object_add(jobj, "age",
		json_object_new_int(age));

	json_object_object_add(jobj, "local",
		json_object_new_boolean(local));

	json_object_object_add(jlist, mac, jobj);
}

static void call_cb_for_fdbs(struct __fdb_entry *fdb, size_t cnt, ifnames_by_port_t *ifnames_by_port, mac_table_entry_cb_t cb, void *arg)
{
	char macbuf[18];

	while (cnt-- > 0) {
		snprintf(macbuf, sizeof(macbuf), "%02x:%02x:%02x:%02x:%02x:%02x",
			fdb->mac_addr[0], fdb->mac_addr[1], fdb->mac_addr[2], fdb->mac_addr[3], fdb->mac_addr[4], fdb->mac_addr[5]);
		macbuf[sizeof(macbuf) - 1] = '\0';

		cb(macbuf, ifnames_by_port->n[fdb->port_no | (fdb->port_hi << 8)], fdb->ageing_timer_value, !!fdb->is_local, arg);
		fdb++;
	}
}

static bool fetch_ifnames(int br_sock, struct ifreq *ifr, ifnames_by_port_t *to)
{
	int port_ifindices[BR_MAX_PORTS];

	unsigned long args[4] = {
		BRCTL_GET_PORT_LIST,
		(uintptr_t) port_ifindices,
		sizeof(port_ifindices) / sizeof(port_ifindices[0]),
		0
	};
	ifr->ifr_data = &args;

	int port_count;

	port_count = ioctl(br_sock, SIOCDEVPRIVATE, ifr);
	if (port_count == -1) {
		printf("GET_PORT_LIST ouch %d %s\n", errno, strerror(errno));
		return false;
	}

	struct ifreq tmp;
	while (port_count-- > 0) {
		if (port_ifindices[port_count] == 0)
			continue;
		tmp.ifr_ifindex = port_ifindices[port_count];
		if (ioctl(br_sock, SIOCGIFNAME, &tmp) == -1) {
			fprintf(stderr, "SIOCGIFNAME ouch %s\n", strerror(errno));
			continue;
		}
		strncpy(to->n[port_count], tmp.ifr_name, IFNAMSIZ);
		to->n[port_count][IFNAMSIZ] = '\0';
	}

	return true;
}

static bool fetch_bridge_mac_table(const char *ifname, mac_table_entry_cb_t cb, void *ctx)
{
	bool res = false;

	int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	if (fd == -1) {
		fprintf(stderr, "open ouch: %s\n", strerror(errno));
		goto cleanup;
	}

	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	strncpy((char *) ifr.ifr_name, ifname, strnlen(ifname, IFNAMSIZ));

	ifnames_by_port_t ifnames_by_port;
	if (!fetch_ifnames(fd, &ifr, &ifnames_by_port)) {
		goto cleanup_fd;
	}

	struct __fdb_entry entries[10];
	unsigned long args[4] = {
		BRCTL_GET_FDB_ENTRIES,
		(uintptr_t) &entries,
		sizeof(entries)/sizeof(entries[0]),
		0
	};
	ifr.ifr_data = &args;

	int fdb_count;

	do {
		fdb_count = ioctl(fd, SIOCDEVPRIVATE, &ifr);
		if (fdb_count == -1) {
			fprintf(stderr, "GET_FDB_ENTRIES ouch: %s\n", strerror(errno));
			goto cleanup_fd;
		}

		call_cb_for_fdbs(entries, fdb_count, &ifnames_by_port, cb, ctx);

		args[3] += args[2];
	} while (fdb_count >= args[2]);

	res = true;

cleanup_fd:
	close(fd);

cleanup:
	return res;
}

int main(int argc, char *argv[])
{
	if (argc < 2) {
		fprintf(stderr, "%s devname\n", argv[0]);
		fprintf(stderr, "set BRMT_JSON environment variable to produce json output\n");
		fprintf(stderr, "set BRMT_DELIM environment variable to set the delimiter\n");
		return -1;
	}

	if (getenv("BRMT_JSON")) {
		struct json_object *jlist = json_object_new_object();

		fetch_bridge_mac_table(argv[1], add_mac_table_entry_to_json, jlist);

		const char *str = json_object_to_json_string(jlist);
		puts(str);
		json_object_put(jlist);
	}
	else {
		char *delim = getenv("BRMT_DELIM");
		if (!delim)
			delim = "\t";
		fetch_bridge_mac_table(argv[1], print_mac_table_entry, delim);
	}

	return 0;
}
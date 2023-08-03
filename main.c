#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
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

static bool read_uint(const char *path, unsigned long *into) {
	int f = open(path, O_RDONLY);
	if (f == -1) {
		fprintf(stderr, "open %s: %s\n", path, strerror(errno));
		return false;
	}

	bool res = false;

	char buf[128];
	ssize_t rlen = read(f, buf, sizeof(buf) - 1);
	if (rlen == -1) {
		fprintf(stderr, "read %s: %s\n", path, strerror(errno));
		goto cleanup;
	}

	buf[rlen] = '\0';

	errno = 0;
	*into = strtoul(buf, NULL, 0);
	if (errno != 0) {
		fprintf(stderr, "error parsing uint '%s': %s\n", buf, strerror(errno));
		goto cleanup;
	}

	res = true;

cleanup:
	close(f);

	return res;
}

static bool fetch_ifnames(const char *bridge, ifnames_by_port_t *to)
{
	// we're in /sys/class/net
	char path_buf[512];
	snprintf(path_buf, sizeof(path_buf), "%s/brif", bridge);

	DIR *d = opendir(path_buf);
	if (!d) {
		fprintf(stderr, "opendir %s: %s\n", path_buf, strerror(errno));
		return false;
	}

	bool res = false;

	struct dirent *dir;
	while ((dir = readdir(d))) {
		const char *name = dir->d_name;
		if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
			continue;

		unsigned long port_no;

		snprintf(path_buf, sizeof(path_buf), "%s/brif/%s/port_no", bridge, name);
		if (!read_uint(path_buf, &port_no))
			goto cleanup;

		if (port_no >= BR_MAX_PORTS) {
			fprintf(stderr, "invalid port_no %s: %lu\n", name, port_no);
			continue;
		}

		strncpy(to->n[port_no], name, IFNAMSIZ);
		to->n[port_no][IFNAMSIZ] = '\0';
	}

	res = true;

cleanup:
	closedir(d);

	return res;
}

static bool fetch_bridge_mac_table(const char *bridge, mac_table_entry_cb_t cb, void *ctx)
{
	char path[256];
	snprintf(path, sizeof(path), "/sys/devices/virtual/net/%s/brforward", bridge);

	int f = open(path, O_RDONLY);
	if (f == -1) {
		fprintf(stderr, "open %s: %s\n", path, strerror(errno));
		return false;
	}

	ifnames_by_port_t ifnames_by_port;
	if (!fetch_ifnames(bridge, &ifnames_by_port))
		return false;

	bool res = false;
	ssize_t rlen;

	while (true) {
		struct __fdb_entry entry;
		rlen = read(f, &entry, sizeof(entry));
		if (rlen != sizeof(entry))
			break;

		char macbuf[18];
		snprintf(macbuf, sizeof(macbuf), "%02x:%02x:%02x:%02x:%02x:%02x",
			entry.mac_addr[0], entry.mac_addr[1], entry.mac_addr[2], entry.mac_addr[3], entry.mac_addr[4], entry.mac_addr[5]);
		macbuf[sizeof(macbuf) - 1] = '\0';

		cb(macbuf, ifnames_by_port.n[entry.port_no | (entry.port_hi << 8)], entry.ageing_timer_value, !!entry.is_local, ctx);
	}

	if (rlen == -1) {
		fprintf(stderr, "read %s: %s\n", path, strerror(errno));
		goto cleanup_fd;
	}

	if (rlen > 0) {
		fprintf(stderr, "short read %s (%zd)\n", path, rlen);
		goto cleanup_fd;
	}

	res = true;

cleanup_fd:
	close(f);

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

	if (chdir("/sys/class/net") != 0) {
		fprintf(stderr, "error changing to /sys/class/net: %s\n", strerror(errno));
		return -1;
	}

	const char *bridge = argv[1];
	if (strlen(bridge) > IFNAMSIZ) {
		fprintf(stderr, "bridge name invalied\n");
		return -1;
	}

	if (getenv("BRMT_JSON")) {
		struct json_object *jlist = json_object_new_object();

		fetch_bridge_mac_table(bridge, add_mac_table_entry_to_json, jlist);

		const char *str = json_object_to_json_string(jlist);
		puts(str);
		json_object_put(jlist);
	}
	else {
		char *delim = getenv("BRMT_DELIM");
		if (!delim)
			delim = "\t";
		fetch_bridge_mac_table(bridge, print_mac_table_entry, delim);
	}

	return 0;
}
/*
 * Minimal HCIDEVUP/HCIGETDEVINFO tool -- this device has no hciconfig,
 * bluetoothctl, or btmgmt on it at all (checked via `which`, all
 * missing), so there's no other way to bring hci0 up or query its real
 * HCI_UP/HCI_RUNNING/HCI_INIT flags after rtk_hciattach hands the line
 * discipline to the kernel's own hci_h5 driver. This does exactly what
 * `hciconfig hci0 up` / `hciconfig hci0` do internally, nothing more.
 *
 * Usage: hci-updown <up|info> [dev_id]   (dev_id default 0 = hci0)
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* Minimal subset of <bluetooth/hci.h>/<bluetooth/bluetooth.h>, copied
 * by value here since the target's toolchain doesn't have bluez-dev
 * headers installed -- these values are from the kernel/BlueZ ABI and
 * are stable across kernel versions. */
#define AF_BLUETOOTH_ 31
#define BTPROTO_HCI_ 1

#define HCIDEVUP_    _IOW('H', 201, int)
#define HCIDEVDOWN_  _IOW('H', 202, int)
#define HCIGETDEVINFO_ _IOR('H', 211, int)
#define HCIGETDEVLIST_ _IOR('H', 210, int)

#define HCI_UP        0
#define HCI_INIT      1
#define HCI_RUNNING   2
#define HCI_PSCAN     3
#define HCI_ISCAN     4
#define HCI_AUTH      5
#define HCI_ENCRYPT   6
#define HCI_INQUIRY   7
#define HCI_RAW       8

struct hci_dev_stats {
	uint32_t err_rx, err_tx, cmd_tx, evt_rx, acl_tx, acl_rx,
		 sco_tx, sco_rx, byte_rx, byte_tx;
};

struct hci_dev_info {
	uint16_t dev_id;
	char name[8];
	uint8_t bdaddr[6];
	uint32_t flags;
	uint8_t type;
	uint8_t features[8];
	uint32_t pkt_type;
	uint32_t link_policy;
	uint32_t link_mode;
	uint16_t acl_mtu, acl_pkts, sco_mtu, sco_pkts;
	struct hci_dev_stats stat;
};

static void print_flags(uint32_t flags)
{
	static const char *names[] = {
		"UP", "INIT", "RUNNING", "PSCAN", "ISCAN",
		"AUTH", "ENCRYPT", "INQUIRY", "RAW"
	};
	int i, first = 1;
	printf("flags: 0x%08x (", flags);
	for (i = 0; i < 9; i++) {
		if (flags & (1u << i)) {
			printf("%s%s", first ? "" : " ", names[i]);
			first = 0;
		}
	}
	if (first)
		printf("none");
	printf(")\n");
}

int main(int argc, char **argv)
{
	const char *cmd;
	int dev_id = 0;
	int sock;
	struct hci_dev_info di;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <up|down|info> [dev_id]\n", argv[0]);
		return 1;
	}
	cmd = argv[1];
	if (argc >= 3)
		dev_id = atoi(argv[2]);

	sock = socket(AF_BLUETOOTH_, SOCK_RAW, BTPROTO_HCI_);
	if (sock < 0) {
		fprintf(stderr, "socket(AF_BLUETOOTH, HCI) failed: %s\n",
			strerror(errno));
		return 1;
	}

	if (strcmp(cmd, "up") == 0) {
		if (ioctl(sock, HCIDEVUP_, dev_id) < 0 && errno != EALREADY) {
			fprintf(stderr, "HCIDEVUP hci%d failed: %d, %s\n",
				dev_id, errno, strerror(errno));
			close(sock);
			return 1;
		}
		/* EALREADY means the device is already up -- the kernel's own
		 * H5 resync can bring hci0 UP RUNNING before this ioctl runs
		 * (real hardware-confirmed 2026-08-23: "info" showed UP
		 * RUNNING with real traffic despite this "failing"), and
		 * that's success, not an error. */
		printf("HCIDEVUP hci%d: OK\n", dev_id);
	} else if (strcmp(cmd, "down") == 0) {
		if (ioctl(sock, HCIDEVDOWN_, dev_id) < 0) {
			fprintf(stderr, "HCIDEVDOWN hci%d failed: %d, %s\n",
				dev_id, errno, strerror(errno));
			close(sock);
			return 1;
		}
		printf("HCIDEVDOWN hci%d: OK\n", dev_id);
	} else if (strcmp(cmd, "info") != 0) {
		fprintf(stderr, "usage: %s <up|down|info> [dev_id]\n", argv[0]);
		close(sock);
		return 1;
	}

	memset(&di, 0, sizeof(di));
	di.dev_id = dev_id;
	if (ioctl(sock, HCIGETDEVINFO_, &di) < 0) {
		fprintf(stderr, "HCIGETDEVINFO hci%d failed: %d, %s\n",
			dev_id, errno, strerror(errno));
		close(sock);
		return 1;
	}

	printf("hci%d (%s): type 0x%02x\n", di.dev_id, di.name, di.type);
	printf("  bdaddr: %02x:%02x:%02x:%02x:%02x:%02x\n",
	       di.bdaddr[5], di.bdaddr[4], di.bdaddr[3],
	       di.bdaddr[2], di.bdaddr[1], di.bdaddr[0]);
	print_flags(di.flags);
	printf("  acl mtu: %u:%u  sco mtu: %u:%u\n",
	       di.acl_mtu, di.acl_pkts, di.sco_mtu, di.sco_pkts);
	printf("  stats: err_rx %u err_tx %u cmd_tx %u evt_rx %u "
	       "acl_tx %u acl_rx %u byte_rx %u byte_tx %u\n",
	       di.stat.err_rx, di.stat.err_tx, di.stat.cmd_tx, di.stat.evt_rx,
	       di.stat.acl_tx, di.stat.acl_rx, di.stat.byte_rx, di.stat.byte_tx);

	close(sock);
	return 0;
}

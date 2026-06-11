#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifndef R503_ARDUINOD_SERIAL
#define R503_ARDUINOD_SERIAL "/dev/ttyUSB0"
#endif

#ifndef R503_ARDUINOD_SOCKET
#define R503_ARDUINOD_SOCKET "/run/r503-arduinod.sock"
#endif

#define IO_BUF_SIZE 4096
#define CLIENT_READ_CHUNK 1024
#define HEALTHCHECK_INTERVAL_MS 5000
#define GENERIC_TIMEOUT_MS 1000
#define FINGER_TIMEOUT_MS 10000

static volatile sig_atomic_t g_quit = 0;

typedef enum {
  CMD_NONE = 0,
  CMD_PING,
  CMD_INFO,
  CMD_ENROLL,
  CMD_VERIFY,
  CMD_LIST,
  CMD_DELETE,
  CMD_CLEAR,
} PendingCmd;

typedef struct {
  const char *serial_path;
  int serial_fd;
  int client_fd;
  char serial_buf[IO_BUF_SIZE];
  size_t serial_len;
  char client_buf[IO_BUF_SIZE];
  size_t client_len;
  PendingCmd pending_cmd;
  bool healthcheck_pending;
  long long next_healthcheck_ms;
  long long healthcheck_deadline_ms;
  long long command_deadline_ms;
} BridgeState;

static void print_help(const char *prog) {
  printf("Usage: %s [--serial PATH] [--socket PATH] [--help]\n", prog);
  printf("  --serial PATH   serial device (default: " R503_ARDUINOD_SERIAL
         ")\n");
  printf("  --socket PATH   AF_UNIX listen path "
         "(default: " R503_ARDUINOD_SOCKET ")\n");
  printf("  --help          show this help\n");
}

static void handle_signal(int sig) {
  (void)sig;
  g_quit = 1;
}

static long long now_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
    return 0;
  return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void trim_eol(char *line) {
  if (!line)
    return;

  size_t len = strlen(line);
  while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
    line[--len] = '\0';
}

static void uppercase_in_place(char *line) {
  if (!line)
    return;

  for (; *line != '\0'; line++)
    *line = (char)toupper((unsigned char)*line);
}

static int write_all(int fd, const char *buf, size_t len) {
  size_t written = 0;
  while (written < len) {
    ssize_t n = write(fd, buf + written, len - written);
    if (n < 0) {
      if (errno == EINTR) {
        if (g_quit)
          return -1;
        continue;
      }
      return -1;
    }
    if (n == 0)
      return -1;
    written += (size_t)n;
  }
  return 0;
}

static bool extract_line(char *buf, size_t *len, char *out, size_t out_cap,
                         size_t *out_len) {
  char *nl = memchr(buf, '\n', *len);
  if (!nl)
    return false;

  size_t line_len = (size_t)(nl - buf) + 1;
  if (line_len >= out_cap)
    return false;

  memcpy(out, buf, line_len);
  out[line_len] = '\0';

  size_t remaining = *len - line_len;
  if (remaining > 0)
    memmove(buf, buf + line_len, remaining);
  *len = remaining;
  *out_len = line_len;
  return true;
}

static int setup_serial(const char *path) {
  int fd = open(path, O_RDWR | O_NOCTTY | O_SYNC);
  if (fd < 0) {
    perror("open(serial)");
    return -1;
  }

  struct termios tty;
  if (tcgetattr(fd, &tty) < 0) {
    perror("tcgetattr");
    close(fd);
    return -1;
  }

  cfmakeraw(&tty);
  tty.c_cflag |= (CLOCAL | CREAD);
  tty.c_cflag &= ~HUPCL;
  tty.c_cc[VMIN] = 1;
  tty.c_cc[VTIME] = 0;

  speed_t speed = B9600;
  if (cfsetspeed(&tty, speed) < 0) {
    perror("cfsetspeed");
    close(fd);
    return -1;
  }

  if (tcsetattr(fd, TCSANOW, &tty) < 0) {
    perror("tcsetattr");
    close(fd);
    return -1;
  }

  tcflush(fd, TCIOFLUSH);
  return fd;
}

static int setup_socket(const char *path) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    perror("socket");
    return -1;
  }

  struct sockaddr_un addr = {
      .sun_family = AF_UNIX,
  };
  size_t path_len = strlen(path);
  if (path_len >= sizeof(addr.sun_path)) {
    fprintf(stderr, "socket path too long\n");
    close(fd);
    return -1;
  }
  memcpy(addr.sun_path, path, path_len + 1);

  unlink(path);

  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("bind");
    close(fd);
    return -1;
  }

  if (listen(fd, 1) < 0) {
    perror("listen");
    close(fd);
    return -1;
  }

  if (chmod(path, 0666) < 0) {
    perror("chmod(socket)");
    close(fd);
    return -1;
  }

  return fd;
}

static void close_client(BridgeState *st) {
  if (st->client_fd >= 0)
    close(st->client_fd);
  st->client_fd = -1;
  st->client_len = 0;
}

static void close_serial(BridgeState *st) {
  if (st->serial_fd >= 0)
    close(st->serial_fd);
  st->serial_fd = -1;
  st->serial_len = 0;
  st->healthcheck_pending = false;
  st->healthcheck_deadline_ms = 0;
  st->command_deadline_ms = 0;
}

static void reset_healthcheck(BridgeState *st, long long now) {
  st->healthcheck_pending = false;
  st->next_healthcheck_ms = now + HEALTHCHECK_INTERVAL_MS;
  st->healthcheck_deadline_ms = 0;
}

static PendingCmd classify_command(const char *line) {
  if (strcmp(line, "PING") == 0)
    return CMD_PING;
  if (strcmp(line, "INFO") == 0)
    return CMD_INFO;
  if (strcmp(line, "ENROLL") == 0)
    return CMD_ENROLL;
  if (strcmp(line, "VERIFY") == 0)
    return CMD_VERIFY;
  if (strcmp(line, "LIST") == 0)
    return CMD_LIST;
  if (strncmp(line, "DELETE:", 7) == 0)
    return CMD_DELETE;
  if (strcmp(line, "CLEAR") == 0)
    return CMD_CLEAR;
  return CMD_NONE;
}

static bool pending_cmd(PendingCmd cmd, const char *line) {
  if (strncmp(line, "ERR:", 4) == 0)
    return true;

  switch (cmd) {
  case CMD_PING:
    return strcmp(line, "OK") == 0;
  case CMD_INFO:
    return strncmp(line, "OK:CAPACITY,", 12) == 0;
  case CMD_ENROLL:
    return strncmp(line, "OK:ENROLLED,", 12) == 0;
  case CMD_VERIFY:
    return strncmp(line, "OK:VERIFIED,", 12) == 0;
  case CMD_LIST:
    return strncmp(line, "OK:LIST", 7) == 0;
  case CMD_DELETE:
    return strncmp(line, "OK:DELETED,", 11) == 0;
  case CMD_CLEAR:
    return strcmp(line, "OK:CLEARED") == 0;
  case CMD_NONE:
  default:
    return false;
  }
}

static bool is_intermediate_step(PendingCmd cmd, const char *line) {
  switch (cmd) {
  case CMD_ENROLL:
    return strncmp(line, "OK:PLACE_FINGER,", 16) == 0 ||
           strcmp(line, "OK:REMOVE_FINGER") == 0;
  case CMD_VERIFY:
    return strcmp(line, "OK:PLACE_FINGER") == 0;
  default:
    return false;
  }
}

static int process_client_line(BridgeState *st, const char *raw, size_t raw_len,
                               long long now) {
  char line[IO_BUF_SIZE];
  if (raw_len >= sizeof(line))
    return -1;

  memcpy(line, raw, raw_len);
  line[raw_len] = '\0';
  trim_eol(line);
  uppercase_in_place(line);

  if (st->serial_fd < 0)
    return -1;

  PendingCmd cmd = classify_command(line);
  if (cmd != CMD_NONE) {
    st->pending_cmd = cmd;
    st->command_deadline_ms = now + GENERIC_TIMEOUT_MS;
  }

  if (write_all(st->serial_fd, raw, raw_len) < 0) {
    perror("write serial");
    return -1;
  }

  return 0;
}

static int process_serial_line(BridgeState *st, const char *raw, size_t raw_len,
                               long long now) {
  char line[IO_BUF_SIZE];
  if (raw_len >= sizeof(line))
    return -1;

  memcpy(line, raw, raw_len);
  line[raw_len] = '\0';
  trim_eol(line);

  if (st->healthcheck_pending) {
    if (strcmp(line, "OK") == 0) {
      st->healthcheck_pending = false;
      st->next_healthcheck_ms = now + HEALTHCHECK_INTERVAL_MS;
      return 0;
    }

    fprintf(stderr, "unexpected healthcheck response: %s\n", line);
    return -1;
  }

  if (st->client_fd >= 0) {
    if (write_all(st->client_fd, raw, raw_len) < 0)
      return -1;
  }

  if (st->pending_cmd != CMD_NONE && pending_cmd(st->pending_cmd, line)) {
    st->pending_cmd = CMD_NONE;
    st->command_deadline_ms = 0;
  } else if (st->pending_cmd != CMD_NONE &&
             is_intermediate_step(st->pending_cmd, line)) {
    st->command_deadline_ms = now + FINGER_TIMEOUT_MS + GENERIC_TIMEOUT_MS;
  }

  return 0;
}

static int drain_fd(BridgeState *st, int fd, bool from_client, long long now) {
  char chunk[CLIENT_READ_CHUNK];
  char line[IO_BUF_SIZE];
  char *buf = from_client ? st->client_buf : st->serial_buf;
  size_t *len = from_client ? &st->client_len : &st->serial_len;

  ssize_t n = read(fd, chunk, sizeof(chunk));
  if (n < 0) {
    if (errno == EINTR)
      return 0;
    return -1;
  }
  if (n == 0)
    return 1;

  if (*len + (size_t)n > IO_BUF_SIZE)
    return -1;

  memcpy(buf + *len, chunk, (size_t)n);
  *len += (size_t)n;

  size_t line_len = 0;
  while (extract_line(buf, len, line, sizeof(line), &line_len)) {
    int rc = from_client ? process_client_line(st, line, line_len, now)
                         : process_serial_line(st, line, line_len, now);
    if (rc < 0)
      return -1;
  }

  return 0;
}

static void init_bridge_state(BridgeState *st, const char *serial_path) {
  *st = (BridgeState){
      .serial_path = serial_path,
      .serial_fd = -1,
      .client_fd = -1,
      .serial_len = 0,
      .client_len = 0,
      .pending_cmd = CMD_NONE,
      .healthcheck_pending = false,
      .next_healthcheck_ms = 0,
      .healthcheck_deadline_ms = 0,
      .command_deadline_ms = 0,
  };
}

static int accept_client(int listen_fd) {
  struct sockaddr_un client_addr;
  socklen_t client_addr_len = sizeof(client_addr);
  return accept(listen_fd, (struct sockaddr *)&client_addr, &client_addr_len);
}

static void update_serial_connection(BridgeState *st, long long now) {
  (void)now;
  if (st->serial_fd >= 0)
    return;

  st->serial_fd = setup_serial(st->serial_path);
  if (st->serial_fd >= 0)
    reset_healthcheck(st, now);
}

static int send_healthcheck(BridgeState *st, long long now) {
  if (st->serial_fd < 0 || st->healthcheck_pending ||
      st->pending_cmd != CMD_NONE || st->client_len != 0 ||
      st->serial_len != 0 || now < st->next_healthcheck_ms)
    return 0;

  if (write_all(st->serial_fd, "PING\n", 5) < 0) {
    perror("write serial");
    return -1;
  }

  st->healthcheck_pending = true;
  st->healthcheck_deadline_ms = now + GENERIC_TIMEOUT_MS;
  return 1;
}

static int compute_poll_timeout(const BridgeState *st, long long now) {
  long long next_ms;

  if (st->serial_fd < 0)
    return 0;
  else if (st->healthcheck_pending)
    next_ms = st->healthcheck_deadline_ms;
  else if (st->pending_cmd != CMD_NONE)
    next_ms = st->command_deadline_ms;
  else
    next_ms = st->next_healthcheck_ms;

  if (next_ms <= now)
    return 0;
  return (int)(next_ms - now);
}

static int exit_bridge(BridgeState *st) {
  close_client(st);
  close_serial(st);
  return 1;
}

static int bridge(const char *serial_path, int listen_fd) {
  BridgeState st;
  init_bridge_state(&st, serial_path);

  while (!g_quit) {
    long long now = now_ms();
    update_serial_connection(&st, now);
    if (send_healthcheck(&st, now) < 0)
      return exit_bridge(&st);

    struct pollfd fds[3];
    nfds_t nfds = 0;
    int listen_idx = -1;
    int serial_idx = -1;
    int client_idx = -1;

    fds[nfds].fd = listen_fd;
    fds[nfds].events = POLLIN;
    fds[nfds].revents = 0;
    listen_idx = (int)nfds;
    nfds++;

    if (st.serial_fd >= 0) {
      fds[nfds].fd = st.serial_fd;
      fds[nfds].events = POLLIN;
      fds[nfds].revents = 0;
      serial_idx = (int)nfds;
      nfds++;
    }

    if (st.client_fd >= 0) {
      fds[nfds].fd = st.client_fd;
      fds[nfds].events = POLLIN;
      fds[nfds].revents = 0;
      client_idx = (int)nfds;
      nfds++;
    }

    int timeout = compute_poll_timeout(&st, now);
    int ret = poll(fds, nfds, timeout);
    if (ret < 0) {
      if (errno == EINTR)
        continue;
      perror("poll");
      break;
    }

    now = now_ms();

    if (st.healthcheck_pending && now >= st.healthcheck_deadline_ms) {
      fprintf(stderr, "healthcheck timeout\n");
      return exit_bridge(&st);
    }

    if (st.pending_cmd != CMD_NONE && now >= st.command_deadline_ms) {
      fprintf(stderr, "command timeout (cmd=%d)\n", st.pending_cmd);
      return exit_bridge(&st);
    }

    if (listen_idx >= 0 && (fds[listen_idx].revents & POLLIN)) {
      int client_fd = accept_client(listen_fd);
      if (client_fd < 0) {
        if (errno != EINTR) {
          perror("accept");
          break;
        }
      } else {
        close_client(&st);
        st.client_fd = client_fd;
      }
    }

    if (serial_idx >= 0 &&
        (fds[serial_idx].revents & (POLLERR | POLLHUP | POLLNVAL))) {
      return exit_bridge(&st);
    }

    if (client_idx >= 0 &&
        (fds[client_idx].revents & (POLLERR | POLLHUP | POLLNVAL))) {
      close_client(&st);
      continue;
    }

    if (serial_idx >= 0 && (fds[serial_idx].revents & POLLIN)) {
      int rc = drain_fd(&st, st.serial_fd, false, now);
      if (rc < 0 || rc > 0)
        return exit_bridge(&st);
    }

    if (client_idx >= 0 && (fds[client_idx].revents & POLLIN)) {
      int rc = drain_fd(&st, st.client_fd, true, now);
      if (rc < 0 || rc > 0) {
        close_client(&st);
        continue;
      }
    }
  }

  return exit_bridge(&st);
}

int main(int argc, char **argv) {
  const char *serial_path = R503_ARDUINOD_SERIAL;
  const char *socket_path = R503_ARDUINOD_SOCKET;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0) {
      print_help(argv[0]);
      return EXIT_SUCCESS;
    } else if (strcmp(argv[i], "--serial") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "error: --serial requires an argument\n");
        return EXIT_FAILURE;
      }
      serial_path = argv[++i];
    } else if (strcmp(argv[i], "--socket") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "error: --socket requires an argument\n");
        return EXIT_FAILURE;
      }
      socket_path = argv[++i];
    } else {
      fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
      print_help(argv[0]);
      return EXIT_FAILURE;
    }
  }

  struct sigaction sa = {
      .sa_handler = handle_signal,
  };
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);

  int listen_fd = setup_socket(socket_path);
  if (listen_fd < 0)
    return EXIT_FAILURE;

  int failed = bridge(serial_path, listen_fd);

  close(listen_fd);
  unlink(socket_path);

  return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}

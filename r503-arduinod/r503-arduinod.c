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
#include <unistd.h>

#ifndef R503_ARDUINOD_SERIAL
#define R503_ARDUINOD_SERIAL "/dev/ttyUSB0"
#endif

#ifndef R503_ARDUINOD_SOCKET
#define R503_ARDUINOD_SOCKET "/run/r503-arduinod.sock"
#endif

static volatile sig_atomic_t g_quit = 0;

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

static int forward(int from_fd, int to_fd) {
  char buf[4096];
  ssize_t n = read(from_fd, buf, sizeof(buf));
  if (n < 0) {
    if (errno == EINTR)
      return 0;
    return -1;
  }
  if (n == 0)
    return -1;
  return write_all(to_fd, buf, (size_t)n);
}

static void bridge(int serial_fd, int listen_fd, const char *socket_path) {
  (void)socket_path;

  while (!g_quit) {
    struct sockaddr_un client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    int client_fd =
        accept(listen_fd, (struct sockaddr *)&client_addr, &client_addr_len);
    if (client_fd < 0) {
      if (errno == EINTR) {
        if (g_quit)
          break;
        continue;
      }
      perror("accept");
      break;
    }

    while (!g_quit) {
      struct pollfd fds[2] = {
          {serial_fd, POLLIN, 0},
          {client_fd, POLLIN, 0},
      };

      int ret = poll(fds, 2, -1);
      if (ret < 0) {
        if (errno == EINTR) {
          if (g_quit)
            break;
          continue;
        }
        perror("poll");
        break;
      }

      if (fds[0].revents & POLLIN) {
        if (forward(serial_fd, client_fd) < 0)
          break;
      }
      if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL))
        break;

      if (fds[1].revents & POLLIN) {
        if (forward(client_fd, serial_fd) < 0)
          break;
      }
      if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL))
        break;
    }

    close(client_fd);
  }
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

  int serial_fd = setup_serial(serial_path);
  if (serial_fd < 0)
    return EXIT_FAILURE;

  int listen_fd = setup_socket(socket_path);
  if (listen_fd < 0) {
    close(serial_fd);
    return EXIT_FAILURE;
  }

  bridge(serial_fd, listen_fd, socket_path);

  close(listen_fd);
  close(serial_fd);
  unlink(socket_path);

  return EXIT_SUCCESS;
}

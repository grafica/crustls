#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <winsock2.h>
  #include <ws2tcpip.h>    /* gai_strerror() */
  #include <io.h>          /* write() */
  #include <fcntl.h>       /* O_BINARY */
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netdb.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
#endif

#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#ifdef _WIN32
  #define sleep(s)        Sleep (1000*(s))
  #define read(s, buf, n) recv (s, buf, n, 0)
  #define close(s)        closesocket(s)
  #define bzero(buf, n)   memset(buf, '\0', n)

  /* Hacks for 'errno' stuff
   */
  #undef  EAGAIN
  #define EAGAIN       WSAEWOULDBLOCK
  #undef  EWOULDBLOCK
  #define EWOULDBLOCK  WSAEWOULDBLOCK
  #undef  errno
  #define errno        WSAGetLastError()
  #define perror(str)  fprintf(stderr, str ": %d.\n", WSAGetLastError())
  #define strerror(e)  ws_strerror(e)
  #ifndef STDOUT_FILENO
  #define STDOUT_FILENO 1  /* MinGW has this */
  #endif
#endif

/* crustls.h is autogenerated in the Makefile using cbindgen. */
#include "crustls.h"

enum crustls_demo_result
{
  CRUSTLS_DEMO_OK,
  CRUSTLS_DEMO_ERROR,
  CRUSTLS_DEMO_AGAIN,
  CRUSTLS_DEMO_EOF,
  CRUSTLS_DEMO_CLOSE_NOTIFY,
};

void
print_error(char *prefix, rustls_result result)
{
  char buf[256];
  size_t n;
  rustls_error(result, buf, sizeof(buf), &n);
  fprintf(stderr, "%s: %.*s\n", prefix, (int)n, buf);
}

#ifdef _WIN32
const char *ws_strerror (int err)
{
  static char ws_err[50];

  if (err >= WSABASEERR) {
    snprintf(ws_err, sizeof(ws_err), "Winsock err: %d", err);
    return ws_err;
  }
  /* Assume a CRT error */
  return (strerror)(err);
}
#endif

/*
 * Write n bytes from buf to the provided fd, retrying short writes until
 * we finish or hit an error. Assumes fd is blocking and therefore doesn't
 * handle EAGAIN. Returns 0 for success or 1 for error.
 *
 * For Winsock we cannot use a socket-fd in write().
 * Call send() if fd > STDOUT_FILENO.
 */
int
write_all(int fd, const char *buf, int n)
{
  int m = 0, must_use_send = 0;

#ifdef _WIN32
  must_use_send = (fd > STDOUT_FILENO);
#endif

  while(n > 0) {
    m = must_use_send ? send(fd, buf, n, 0) : write(fd, buf, n);
    if(m < 0) {
      must_use_send ? perror("writing to socket") : perror("writing to stdout");
      return 1;
    }
    if(m == 0) {
      fprintf(stderr, "early EOF when writing to %s\n", must_use_send ? "socket" : "stdout");
      return 1;
    }
    n -= m;
  }
  return 0;
}

/*
 * Set a socket to be nonblocking.
 *
 * Returns CRUSTLS_DEMO_OK on success, CRUSTLS_DEMO_ERROR on error.
 */
enum crustls_demo_result
nonblock(int sockfd)
{
#ifdef _WIN32
  u_long nonblock = 1UL;

  if (ioctlsocket(sockfd, FIONBIO, &nonblock) != 0) {
    perror("Error setting socket nonblocking");
    return CRUSTLS_DEMO_ERROR;
  }
#else
  int flags;
  flags = fcntl(sockfd, F_GETFL, 0);
  if(flags < 0) {
    perror("getting socket flags");
    return CRUSTLS_DEMO_ERROR;
  }
  flags = fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
  if(flags < 0) {
    perror("setting socket nonblocking");
    return CRUSTLS_DEMO_ERROR;
  }
#endif
  return CRUSTLS_DEMO_OK;
}

/*
 * Connect to the given hostname on port 443 and return the file descriptor of
 * the socket. On error, print the error and return 1. Caller is responsible
 * for closing socket.
 */
int
make_conn(const char *hostname)
{
  int sockfd = 0;
  enum crustls_demo_result result = 0;
  struct addrinfo *getaddrinfo_output = NULL, hints;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM; /* looking for TCP */

  int getaddrinfo_result =
    getaddrinfo(hostname, "443", &hints, &getaddrinfo_output);
  if(getaddrinfo_result != 0) {
    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(getaddrinfo_result));
    goto cleanup;
  }

  sockfd = socket(getaddrinfo_output->ai_family,
                  getaddrinfo_output->ai_socktype,
                  getaddrinfo_output->ai_protocol);
  if(sockfd < 0) {
    perror("making socket");
    goto cleanup;
  }

  int connect_result = connect(
    sockfd, getaddrinfo_output->ai_addr, getaddrinfo_output->ai_addrlen);
  if(connect_result < 0) {
    perror("connecting");
    goto cleanup;
  }
  result = nonblock(sockfd);
  if(result != CRUSTLS_DEMO_OK) {
    return 1;
  }

  freeaddrinfo(getaddrinfo_output);
  return sockfd;

cleanup:
  if(getaddrinfo_output != NULL) {
    freeaddrinfo(getaddrinfo_output);
  }
  if(sockfd > 0) {
    close(sockfd);
  }
  return -1;
}

/*
 * Copy all the ciphertext bytes from buf into the client session.
 * Returns 0 for success, 1 for error.
 */
int
copy_tls_bytes_into_client_session(
  struct rustls_client_session *client_session, uint8_t *buf, size_t len,
  size_t *out_n)
{
  size_t n;
  size_t n_written = 0;
  int result;

  while(n_written < len) {
    result = rustls_client_session_read_tls(
      client_session, buf + n_written, len - n_written, &n);
    if(result != RUSTLS_RESULT_OK) {
      fprintf(stderr, "Error in ClientSession::read_tls\n");
      goto fail;
    }
    if(n == 0) {
      fprintf(stderr, "EOF from ClientSession::read_tls\n");
      goto fail;
    }
    if(n > len) {
      fprintf(stderr, "too many bytes written to ClientSession; overflow\n");
      goto fail;
    }
    n_written += n;

    result = rustls_client_session_process_new_packets(client_session);
    if(result != RUSTLS_RESULT_OK) {
      print_error("in process_new_packets", result);
      goto fail;
    }
  }
  *out_n = n_written;
  return 0;

fail:
  *out_n = n_written;
  return 1;
}

/* Read all available bytes from the client_session until EOF.
 * Note that EOF here indicates "no more bytes until
 * process_new_packets", not "stream is closed".
 *
 * Returns CRUSTLS_DEMO_OK for success,
 * CRUSTLS_DEMO_ERROR for error,
 * CRUSTLS_DEMO_CLOSE_NOTIFY for "received close_notify"
 */
int
copy_plaintext_to_stdout(struct rustls_client_session *client_session)
{
  int result;
  char buf[2048];
  size_t n;

  for(;;) {
    bzero(buf, sizeof(buf));
    result = rustls_client_session_read(
      client_session, (uint8_t *)buf, sizeof(buf), &n);
    if(result == RUSTLS_RESULT_ALERT_CLOSE_NOTIFY) {
      fprintf(stderr, "Received close_notify, cleanly ending connection\n");
      return CRUSTLS_DEMO_CLOSE_NOTIFY;
    }
    if(result != RUSTLS_RESULT_OK) {
      fprintf(stderr, "Error in ClientSession::read\n");
      return CRUSTLS_DEMO_ERROR;
    }
    if(n == 0) {
      /* EOF from ClientSession::read. This is expected. */
      return CRUSTLS_DEMO_OK;
    }

    result = write_all(STDOUT_FILENO, buf, n);
    if(result != 0) {
      return CRUSTLS_DEMO_ERROR;
    }
  }

  fprintf(stderr, "copy_plaintext_to_stdout: fell through loop\n");
  return CRUSTLS_DEMO_ERROR;
}

/*
 * Do one read from the socket, and process all resulting bytes into the
 * client_session, then copy all plaintext bytes from the session to stdout.
 * Returns:
 *  - CRUSTLS_DEMO_OK for success
 *  - CRUSTLS_DEMO_AGAIN if we got an EAGAIN or EWOULDBLOCK reading from the
 *    socket
 *  - CRUSTLS_DEMO_EOF if we got EOF
 *  - CRUSTLS_DEMO_ERROR for other errors.
 */
enum crustls_demo_result
do_read(int sockfd, struct rustls_client_session *client_session)
{
  int result = 1;
  ssize_t n = 0;
  size_t buflen = 0;
  size_t n_from_rustls = 0;
  char buf[2048];

  bzero(buf, sizeof(buf));
  n = read(sockfd, buf, sizeof(buf));
  if(n == 0) {
    fprintf(stderr, "EOF reading from socket\n");
    return CRUSTLS_DEMO_EOF;
  }
  else if(n < 0) {
    if(errno == EAGAIN || errno == EWOULDBLOCK) {
      fprintf(stderr,
              "reading from socket: EAGAIN or EWOULDBLOCK: %s\n",
              strerror(errno));
      return CRUSTLS_DEMO_AGAIN;
    }
    else {
      perror("reading from socket");
      return CRUSTLS_DEMO_ERROR;
    }
  }
  buflen = (size_t)n;

  /*
   * Now pull those bytes from the buffer into ClientSession.
   * Note that we pass buf, n; not buf, sizeof(buf). We don't
   * want to pull in unitialized memory that we didn't just
   * read from the socket.
   */
  result = copy_tls_bytes_into_client_session(
    client_session, (uint8_t *)buf, buflen, &n_from_rustls);
  if(result != 0) {
    return CRUSTLS_DEMO_ERROR;
  }

  result = copy_plaintext_to_stdout(client_session);
  if(result != CRUSTLS_DEMO_CLOSE_NOTIFY) {
    return result;
  }

  /* If we got a close_notify, verify that the sender then
   * closed the TCP connection. */
  n = read(sockfd, buf, sizeof(buf));
  if(n != 0) {
    fprintf(stderr, "read returned %ld after receiving close_notify\n", n);
    return CRUSTLS_DEMO_ERROR;
  }
  return CRUSTLS_DEMO_CLOSE_NOTIFY;
}

/*
 * Given an established TCP connection, and a rustls client_session, send an
 * HTTP request and read the response. On success, return 0. On error, print
 * the message and return 1.
 */
int
send_request_and_read_response(int sockfd,
                               struct rustls_client_session *client_session,
                               const char *hostname, const char *path)
{
  int ret = 1;
  int result = 1;
  char buf[2048];
  fd_set read_fds;
  fd_set write_fds;
  size_t n = 0;

  bzero(buf, sizeof(buf));
  snprintf(buf,
           sizeof(buf),
           "GET %s HTTP/1.1\r\n"
           "Host: %s\r\n"
           "User-Agent: crustls-demo\r\n"
           "Accept: carcinization/inevitable, text/html\r\n"
           "Connection: close\r\n"
           "\r\n",
           path,
           hostname);
  result = rustls_client_session_write(
    client_session, (uint8_t *)buf, strlen(buf), &n);
  if(result != RUSTLS_RESULT_OK) {
    fprintf(stderr, "error writing plaintext bytes to ClientSession\n");
    goto cleanup;
  }
  if(n != strlen(buf)) {
    fprintf(stderr, "short write writing plaintext bytes to ClientSession\n");
    goto cleanup;
  }

  for(;;) {
    FD_ZERO(&read_fds);
    FD_SET(sockfd, &read_fds);
    FD_ZERO(&write_fds);
    FD_SET(sockfd, &write_fds);

    result = select(sockfd + 1, &read_fds, &write_fds, NULL, NULL);
    if(result == -1) {
      perror("select");
      goto cleanup;
    }

    if(rustls_client_session_wants_read(client_session) &&
       FD_ISSET(sockfd, &read_fds)) {
      fprintf(stderr,
              "ClientSession wants us to read_tls. First we need to pull some "
              "bytes from the socket\n");

      /* Read all bytes until we get EAGAIN. Then loop again to wind up in
         select awaiting the next bit of data. */
      for(;;) {
        result = do_read(sockfd, client_session);
        if(result == CRUSTLS_DEMO_AGAIN) {
          break;
        }
        else if(result == CRUSTLS_DEMO_CLOSE_NOTIFY) {
          ret = 0;
          goto cleanup;
        }
        else if(result != CRUSTLS_DEMO_OK) {
          goto cleanup;
        }
      }
    }
    if(rustls_client_session_wants_write(client_session) &&
       FD_ISSET(sockfd, &write_fds)) {
      fprintf(stderr, "ClientSession wants us to write_tls.\n");
      bzero(buf, sizeof(buf));
      result = rustls_client_session_write_tls(
        client_session, (uint8_t *)buf, sizeof(buf), &n);
      if(result != RUSTLS_RESULT_OK) {
        fprintf(stderr, "Error in ClientSession::write_tls\n");
        goto cleanup;
      }
      else if(n == 0) {
        fprintf(stderr, "EOF from ClientSession::write_tls\n");
        goto cleanup;
      }

      result = write_all(sockfd, buf, n);
      if(result != 0) {
        goto cleanup;
      }
    }
  }

  fprintf(stderr, "send_request_and_read_response: loop fell through");

cleanup:
  if(sockfd > 0) {
    close(sockfd);
  }
  return ret;
}

int
do_request(const struct rustls_client_config *client_config,
           const char *hostname, const char *path)
{
  struct rustls_client_session *client_session = NULL;
  int ret = 1;
  int sockfd = make_conn(hostname);
  if(sockfd < 0) {
    // No perror because make_conn printed error already.
    goto cleanup;
  }

  rustls_result result =
    rustls_client_session_new(client_config, hostname, &client_session);
  if(result != RUSTLS_RESULT_OK) {
    print_error("client_session_new", result);
    goto cleanup;
  }

  ret = send_request_and_read_response(sockfd, client_session, hostname, path);
  if(ret != RUSTLS_RESULT_OK) {
    goto cleanup;
  }

  ret = 0;

cleanup:
  rustls_client_session_free(client_session);
  if(sockfd > 0) {
    close(sockfd);
  }
  return ret;
}

enum rustls_result
verify(void *userdata, const rustls_verify_server_cert_params *params) {
  size_t i = 0;
  const rustls_slice_slice_bytes *intermediates = params->intermediate_certs_der;
  struct rustls_slice_bytes bytes;
  const size_t intermediates_len = rustls_slice_slice_bytes_len(intermediates);

  fprintf(stderr, "custom certificate verifier called for %.*s\n",
    (int)params->dns_name.len, params->dns_name.data);
  fprintf(stderr, "end entity len: %ld\n", params->end_entity_cert_der.len);
  fprintf(stderr, "intermediates:\n");
  for(i = 0; i<intermediates_len; i++) {
    bytes = rustls_slice_slice_bytes_get(intermediates, i);
    if(bytes.data != NULL) {
      fprintf(stderr, "  intermediate, len = %ld\n", bytes.len);
    }
  }
  fprintf(stderr, "ocsp response len: %ld\n", params->ocsp_response.len);
  if(0 != strcmp((const char *)userdata, "verify_arg")) {
    fprintf(stderr, "invalid argument to verify: %p\n", userdata);
    return RUSTLS_RESULT_GENERAL;
  }
  return RUSTLS_RESULT_OK;
}

int
main(int argc, const char **argv)
{
  int ret = 1;
  int result = 1;
  if(argc <= 2) {
    fprintf(stderr,
            "usage: %s hostname path\n\n"
            "Connect to a host via HTTPS on port 443, make a request for the\n"
            "given path, and emit response to stdout.\n",
            argv[0]);
    return 1;
  }
  const char *hostname = argv[1];
  const char *path = argv[2];

  struct rustls_client_config_builder *config_builder =
    rustls_client_config_builder_new();
  const struct rustls_client_config *client_config = NULL;

#ifdef _WIN32
  WSADATA wsa;
  WSAStartup (MAKEWORD(1,1), &wsa);
  setmode(STDOUT_FILENO, O_BINARY);
#endif

  result = rustls_client_config_builder_load_native_roots(config_builder);
  if(result != RUSTLS_RESULT_OK) {
    print_error("loading trusted certificate", result);
    goto cleanup;
  }

  if(getenv("NO_CHECK_CERTIFICATE")) {
    rustls_client_config_builder_dangerous_set_certificate_verifier(config_builder, verify,
      "verify_arg");
  }

  client_config = rustls_client_config_builder_build(config_builder);

  int i;
  for(i = 0; i < 3; i++) {
    result = do_request(client_config, hostname, path);
    if(result != 0) {
      goto cleanup;
    }
  }

  // Success!
  ret = 0;

cleanup:
  rustls_client_config_free(client_config);

#ifdef _WIN32
  WSACleanup();
#endif

  return ret;
}

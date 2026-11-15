/*
 * The host's end of a serial port: where ser1 (COM1) goes and what comes in.
 *
 *   FILE            output appended to a file
 *   tcp:PORT        a null-modem cable on a TCP port: openql listens on
 *                   127.0.0.1:PORT and takes one connection at a time
 *   tcp:HOST:PORT   ... or connects to one (another openql, a terminal)
 *
 * Input can also come from a file (hostser_input_file), sent once.
 */
#ifndef HOSTSER_H
#define HOSTSER_H

#include <stdint.h>
#include <stdio.h>

typedef struct hostser hostser;

hostser *hostser_open(const char *spec);          /* NULL with a message on failure */
int hostser_input_file(hostser **h, const char *path);   /* makes *h if it is NULL */
void hostser_put(hostser *h, uint8_t ch);          /* a byte the QL sent */
int hostser_get(hostser *h);                       /* a byte for the QL, or -1: none now */
void hostser_unget(hostser *h, int ch);            /* the QL had no room: keep it for later */
void hostser_poll(hostser *h);                     /* accept a connection, notice a closed one */
void hostser_close(hostser *h);

#endif

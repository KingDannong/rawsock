/*  Copyright (C) 2013  P.D. Buchan (pdbuchan@yahoo.com)

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// Send an IPv6 TCP packet via raw socket at the link layer (ethernet frame).
// with a large payload requiring fragmentation. Include a hop-by-hop options
// extension header with a router alert option, and a (last) destination
// extension header with an ILNP (identifier-locator network protocol) nonce option.
// Need to have destination MAC address.

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>           // close()
#include <string.h>           // strcpy, memset(), and memcpy()

#include <netdb.h>            // struct addrinfo
#include <sys/types.h>        // needed for socket(), uint8_t, uint16_t, uint32_t
#include <sys/socket.h>       // needed for socket()
#include <netinet/in.h>       // IPPROTO_HOPOPTS, IPPROTO_TCP, IPPROTO_FRAGMENT, INET6_ADDRSTRLEN
#include <netinet/ip.h>       // IP_MAXPACKET (which is 65535)
#include <netinet/ip6.h>      // struct ip6_hdr
#define __FAVOR_BSD           // Use BSD format of tcp header
#include <netinet/tcp.h>      // struct tcphdr
#include <arpa/inet.h>        // inet_pton() and inet_ntop()
#include <sys/ioctl.h>        // macro ioctl is defined
#include <bits/ioctls.h>      // defines values for argument "request" of ioctl.
#include <net/if.h>           // struct ifreq
#include <linux/if_ether.h>   // ETH_P_IP = 0x0800, ETH_P_IPV6 = 0x86DD
#include <linux/if_packet.h>  // struct sockaddr_ll (see man 7 packet)
#include <net/ethernet.h>

#include <errno.h>            // errno, perror()

// Define a struct for hop-by-hop header, excluding options.
typedef struct _hop_hdr hop_hdr;
struct _hop_hdr {
  uint8_t nxt_hdr;
  uint8_t hdr_len;
};

// Define a struct for destination header, excluding options.
typedef struct _dst_hdr dst_hdr;
struct _dst_hdr {
  uint8_t nxt_hdr;
  uint8_t hdr_len;
};

// Define some constants.
#define ETH_HDRLEN 14         // Ethernet header length
#define IP6_HDRLEN 40         // IPv6 header length
#define HOP_HDRLEN 2          // Hop-by-hop header length, excluding options
#define DST_HDRLEN 2          // Destination header length, excluding options
#define TCP_HDRLEN 20         // TCP header length, excludes options data
#define FRG_HDRLEN 8          // IPv6 fragment header
#define MAX_FRAGS 3119        // Maximum number of packet fragments
#define MAX_HBHOPTIONS 10     // Maximum number of hop-by-hop extension header options
#define MAX_HBHOPTLEN 256     // Maximum length of a hop-by-hop option (some large value)
#define MAX_DSTOPTIONS 10     // Maximum number of destination extension header options
#define MAX_DSTOPTLEN 256     // Maximum length of a destination option (some large value)

// Function prototypes
uint16_t checksum (uint16_t *, int);
uint16_t tcp6_checksum (struct ip6_hdr, struct tcphdr, uint8_t *, int);
int option_pad (int *, uint8_t *, int *, int, int);
char *allocate_strmem (int);
uint8_t *allocate_ustrmem (int);
uint8_t **allocate_ustrmemp (int);
int *allocate_intmem (int);

int
main (int argc, char **argv)
{
  int i, j, n, indx, status, frame_length, sd, bytes;
  int hoplen, dstlen, mtu, *frag_flags, *tcp_flags, c, nframes, offset[MAX_FRAGS], len[MAX_FRAGS];
  hop_hdr hophdr;
  int hbh_nopt;  // Number of hop-by-hop options
  int hbh_opt_totlen;  // Total length of hop-by-hop options
  int *hbh_optlen;  // Hop-by-hop option length: hbh_optlen[option #] = int
  uint8_t **hbh_options;  // Hop-by-hop options data: hbh_options[option #] = uint8_t *
  int *hbh_x, *hbh_y;  // Alignment requirements for hop-by-hop options: hbh_x[option #] = int, hbh_y[option #] = int
  int hbh_optpadlen;
  dst_hdr dsthdr;
  int dst_nopt;  // Number of destination options
  int dst_opt_totlen;  // Total length of destination options
  int *dst_optlen;  // Destination option length: dst_optlen[option #] = int
  uint8_t **dst_options;  // Destination options data: dst_options[option #] = uint8_t *
  int *dst_x, *dst_y;  // Alignment requirements for destination options: dst_x[option #] = int, dst_y[option #] = int
  int dst_optpadlen;
  char *interface, *target, *src_ip, *dst_ip;
  struct ip6_hdr iphdr;
  struct tcphdr tcphdr;
  struct ip6_frag fraghdr;
  int payloadlen, fragbufferlen;
  uint8_t *payload, *fragbuffer, *src_mac, *dst_mac, *ether_frame;
  struct addrinfo hints, *res;
  struct sockaddr_in6 *ipv6;
  struct sockaddr_ll device;
  struct ifreq ifr;
  void *tmp;
  FILE *fi;

  // Allocate memory for various arrays.
  hbh_optlen = allocate_intmem (MAX_HBHOPTIONS);  // hbh_optlen[option #] = int
  hbh_options = allocate_ustrmemp (MAX_HBHOPTIONS);  // hbh_options[option #] = uint8_t *
  for (i=0; i<MAX_HBHOPTIONS; i++) {
    hbh_options[i] = allocate_ustrmem (MAX_HBHOPTLEN);
  }
  hbh_x = allocate_intmem (MAX_HBHOPTIONS);  // Hop-by-hop option alignment requirement x (of xN + y): hbh_x[option #] = int
  hbh_y = allocate_intmem (MAX_HBHOPTIONS);  // Hop-by-hop option alignment requirement y (of xN + y): hbh_y[option #] = int
  dst_optlen = allocate_intmem (MAX_DSTOPTIONS);  // dst_optlen[option #] = int
  dst_options = allocate_ustrmemp (MAX_DSTOPTIONS);  // dst_options[option #] = uint8_t *
  for (i=0; i<MAX_DSTOPTIONS; i++) {
    dst_options[i] = allocate_ustrmem (MAX_DSTOPTLEN);
  }
  dst_x = allocate_intmem (MAX_DSTOPTIONS);  // Destination option alignment requirement x (of xN + y): dst_x[option #] = int
  dst_y = allocate_intmem (MAX_DSTOPTIONS);  // Destination option alignment requirement y (of xN + y): dst_y[option #] = int
  src_mac = allocate_ustrmem (6);
  dst_mac = allocate_ustrmem (6);
  ether_frame = allocate_ustrmem (IP_MAXPACKET);
  interface = allocate_strmem (40);
  target = allocate_strmem (INET6_ADDRSTRLEN);
  src_ip = allocate_strmem (INET6_ADDRSTRLEN);
  dst_ip = allocate_strmem (INET6_ADDRSTRLEN);
  tcp_flags = allocate_intmem (8);
  payload = allocate_ustrmem (IP_MAXPACKET);
  frag_flags = allocate_intmem (2);

  // Interface to send packet through.
  strcpy (interface, "eth0");

  // Submit request for a socket descriptor to look up interface.
  if ((sd = socket (PF_PACKET, SOCK_RAW, htons (ETH_P_ALL))) < 0) {
    perror ("socket() failed to get socket descriptor for using ioctl() ");
    exit (EXIT_FAILURE);
  }

  // Use ioctl() to get interface maximum transmission unit (MTU).
  memset (&ifr, 0, sizeof (ifr));
  strcpy (ifr.ifr_name, interface);
  if (ioctl (sd, SIOCGIFMTU, &ifr) < 0) {
    perror ("ioctl() failed to get MTU ");
    return (EXIT_FAILURE);
  }
  mtu = ifr.ifr_mtu;
  printf ("Current MTU of interface %s is: %i\n", interface, mtu);

  // Use ioctl() to look up interface name and get its MAC address.
  memset (&ifr, 0, sizeof (ifr));
  snprintf (ifr.ifr_name, sizeof (ifr.ifr_name), "%s", interface);
  if (ioctl (sd, SIOCGIFHWADDR, &ifr) < 0) {
    perror ("ioctl() failed to get source MAC address ");
    return (EXIT_FAILURE);
  }
  close (sd);

  // Copy source MAC address.
  memcpy (src_mac, ifr.ifr_hwaddr.sa_data, 6 * sizeof (uint8_t));

  // Report source MAC address to stdout.
  printf ("MAC address for interface %s is ", interface);
  for (i=0; i<5; i++) {
    printf ("%02x:", src_mac[i]);
  }
  printf ("%02x\n", src_mac[5]);

  // Find interface index from interface name and store index in
  // struct sockaddr_ll device, which will be used as an argument of sendto().
  if ((device.sll_ifindex = if_nametoindex (interface)) == 0) {
    perror ("if_nametoindex() failed to obtain interface index ");
    exit (EXIT_FAILURE);
  }
  printf ("Index for interface %s is %i\n", interface, device.sll_ifindex);

  // Set destination MAC address: you need to fill these out
  dst_mac[0] = 0xff;
  dst_mac[1] = 0xff;
  dst_mac[2] = 0xff;
  dst_mac[3] = 0xff;
  dst_mac[4] = 0xff;
  dst_mac[5] = 0xff;

  // Source IPv6 address: you need to fill this out
  strcpy (src_ip, "2001:db8::214:51ff:fe2f:1556");

  // Destination URL or IPv6 address: you need to fill this out
  strcpy (target, "ipv6.google.com");

  // Number of hop-by-hop extension header options.
  hbh_nopt = 1;

  // Hop-by-hop option: router alert (with bogus value)
  // Alignment requirement is 2n+0 for router alert. See Section 2.1 of RFC 2711.
  hbh_x[0] = 2;
  hbh_y[0] = 0;
  // hbh_options[option #] = uint8_t *
  hbh_options[0][0] = 5;  // Option Type: router alert
  hbh_options[0][1] = 2;  // Length of Option Data field
  hbh_options[0][2] = 0;  // Option Data: some unassigned IANA value, you
  hbh_options[0][3] = 5;  // should select what you want.
  // Hop-by-hop option length.
  hbh_optlen[0] = 4;  // Hop-by-hop header option length (excludes hop-by-hop header itself (2 bytes))

  // Calculate total length of hop-by-hop options.
  hbh_opt_totlen = 0;
  for (i=0; i<hbh_nopt; i++) {
    hbh_opt_totlen += hbh_optlen[i];
  }

  // Determine total padding needed to align and pad hop-by-hop options (Section 4.2 of RFC 2460).
  indx = 0;
  if (hbh_nopt > 0) {
    indx += HOP_HDRLEN; // Account for hop-by-hop header (Next Header and Header Length)
    for (i=0; i<hbh_nopt; i++) {
      // Add any necessary alignment for option i
      while ((indx % hbh_x[i]) != hbh_y[i]) {
        indx++;
      }
      // Add length of option i
      indx += hbh_optlen[i];
    }
    // Now pad last option to next 8-byte boundary (Section 4.2 of RFC 2460).
    while ((indx % 8) != 0) {
      indx++;
    }

    // Total of alignments and final padding = indx - HOP_HDRLEN - total length of hop-by-hop (non-pad) options
    hbh_optpadlen = indx - HOP_HDRLEN - hbh_opt_totlen;

    // Determine length of hop-by-hop header in units of 8 bytes, excluding first 8 bytes.
    // Section 4.3 of RFC 2460.
    i = (indx - 8) / 8;
    if (i < 0) {
      i = 0;
    }
    hophdr.hdr_len = i;
  } else {
    hbh_opt_totlen = 0;
    hbh_optpadlen = 0;
  }

  // Print some information about hop-by-hop options.
  printf ("Number of hop-by-hop options: %i\n", hbh_nopt);
  printf ("Total length of hop-by-hop options, excluding 2-byte hop-by-hop header and padding: %i\n", hbh_opt_totlen);
  printf ("Total length of hop-by-hop alignment padding and end-padding: %i\n", hbh_optpadlen);

  // Number of destination extension header options.
  dst_nopt = 1;

  // Destination option: Identifier-locator network protocol (ILNP) nonce option
  // Alignment requirement is 4n+2 for ILNP nonce, so that nonce itself, starts on a 4-byte boundary.
  // See Section 2 of RFC 6744. Nonce can be 4 bytes or 12 bytes long. We use 12 bytes here.
  dst_x[0] = 4;
  dst_y[0] = 2;
  // dst_options[option #] = uint8_t *
  dst_options[0][0] = 139;  // Option Type: ILNP nonce
  dst_options[0][1] = 12;  // Length of nonce, in bytes
  dst_options[0][2] = 4;  // Some unique, unpredicable 12-byte number
  dst_options[0][3] = 35;
  dst_options[0][4] = 229;
  dst_options[0][5] = 0;
  dst_options[0][6] = 79;
  dst_options[0][7] = 50;
  dst_options[0][8] = 211;
  dst_options[0][9] = 23;
  dst_options[0][10] = 156;
  dst_options[0][11] = 170;
  dst_options[0][12] = 102;
  dst_options[0][13] = 116;
  // Destination option length.
  dst_optlen[0] = 14;  // Destination header option length (excludes destination header itself (2 bytes))

  // Calculate total length of destination options.
  dst_opt_totlen = 0;
  for (i=0; i<dst_nopt; i++) {
    dst_opt_totlen += dst_optlen[i];
  }

  // Determine total padding needed to align and pad destination options (Section 4.2 of RFC 2460).
  indx = 0;
  if (dst_nopt > 0) {
    indx += DST_HDRLEN; // Account for destination header (Next Header and Header Length)
    for (i=0; i<dst_nopt; i++) {
      // Add any necessary alignment for option i
      while ((indx % dst_x[i]) != dst_y[i]) {
        indx++;
      }
      // Add length of option i
      indx += dst_optlen[i];
    }
    // Now pad last option to next 8-byte boundary (Section 4.2 of RFC 2460).
    while ((indx % 8) != 0) {
      indx++;
    }

    // Total of alignments and final padding = indx - DST_HDRLEN - total length of destination (non-pad) options
    dst_optpadlen = indx - DST_HDRLEN - dst_opt_totlen;

    // Determine length of destination header in units of 8 bytes, excluding first 8 bytes.
    // Section 4.3 of RFC 2460.
    i = (indx - 8) / 8;
    if (i < 0) {
      i = 0;
    }
    dsthdr.hdr_len = i;
  } else {
    dst_opt_totlen = 0;
    dst_optpadlen = 0;
  }

  // Print some information about destination options.
  printf ("Number of destination options: %i\n", dst_nopt);
  printf ("Total length of destination options, excluding 2-byte destination header and padding: %i\n", dst_opt_totlen);
  printf ("Total length of destination alignment padding and end-padding: %i\n", dst_optpadlen);

  // Fill out hints for getaddrinfo().
  memset (&hints, 0, sizeof (struct addrinfo));
  hints.ai_family = AF_INET6;
  hints.ai_socktype = SOCK_RAW;
  hints.ai_flags = hints.ai_flags | AI_CANONNAME;

  // Resolve target using getaddrinfo().
  if ((status = getaddrinfo (target, NULL, &hints, &res)) != 0) {
    fprintf (stderr, "getaddrinfo() failed: %s\n", gai_strerror (status));
    exit (EXIT_FAILURE);
  }
  ipv6 = (struct sockaddr_in6 *) res->ai_addr;
  tmp = &(ipv6->sin6_addr);
  if (inet_ntop (AF_INET6, tmp, dst_ip, INET6_ADDRSTRLEN) == NULL) {
    status = errno;
    fprintf (stderr, "inet_ntop() failed.\nError message: %s", strerror (status));
    exit (EXIT_FAILURE);
  }
  freeaddrinfo (res);

  // Fill out sockaddr_ll.
  device.sll_family = AF_PACKET;
  memcpy (device.sll_addr, src_mac, 6 * sizeof (uint8_t));
  device.sll_halen = htons (6);

  // Get TCP data.
  i = 0;
  fi = fopen ("data", "r");
  if (fi == NULL) {
    printf ("Can't open file 'data'.\n");
    exit (EXIT_FAILURE);
  }
  while ((n=fgetc (fi)) != EOF) {
    payload[i] = n;
    i++;
  }
  fclose (fi);
  payloadlen = i;
  printf ("Upper layer protocol header length (bytes): %i\n", TCP_HDRLEN);
  printf ("Payload length (bytes): %i\n", payloadlen);

  // Length of destination header, options, and padding.
  if (dst_nopt > 0) {
    dstlen = DST_HDRLEN + dst_opt_totlen + dst_optpadlen;
  } else {
    dstlen = 0;
  }

  // Length of fragmentable portion of packet. Destination (last) header is last
  // of all extension headers, and therefore in fragmental portion.
  fragbufferlen = dstlen + TCP_HDRLEN + payloadlen;
  printf ("Total fragmentable data (bytes): %i\n", fragbufferlen);

  // Allocate memory for the fragmentable portion.
  fragbuffer = allocate_ustrmem (fragbufferlen);

  // Length of hop-by-hop header, options, and padding.
  if (hbh_nopt > 0) {
    hoplen = HOP_HDRLEN + hbh_opt_totlen + hbh_optpadlen;
  } else {
    hoplen = 0;
  }

  // Determine how many ethernet frames we'll need.
  // Hop-by-hop header and its options are part of unfragmentable portion of packet.
  memset (len, 0, MAX_FRAGS * sizeof (int));
  memset (offset, 0, MAX_FRAGS * sizeof (int));
  i = 0;
  c = 0;  // Variable c is index to buffer, which contains upper layer protocol header and data.
  while (c < fragbufferlen) {

    // Do we still need to fragment remainder of fragmentable portion?
    if ((fragbufferlen - c) > (mtu - IP6_HDRLEN - hoplen - FRG_HDRLEN)) {  // Yes
      len[i] = mtu - IP6_HDRLEN - hoplen - FRG_HDRLEN;  // len[i] is amount of fragmentable part we can include in this frame.

    } else {  // No
      len[i] = fragbufferlen - c;  // len[i] is amount of fragmentable part we can include in this frame.
    }
    c += len[i];

    // If not last fragment, make sure we have an even number of 8-byte blocks.
    // Reduce length as necessary.
    if (c < (fragbufferlen - 1)) {
      while ((len[i]%8) > 0) {
        len[i]--;
        c--;
      }
    }
    printf ("Frag: %i,  Data (bytes): %i,  Data Offset (8-byte blocks): %i\n", i, len[i], offset[i]);
    i++;
    offset[i] = (len[i-1] / 8) + offset[i-1];
  }
  nframes = i;
  printf ("Total number of frames to send: %i\n", nframes);

  // IPv6 header

  // IPv6 version (4 bits), Traffic class (8 bits), Flow label (20 bits)
  iphdr.ip6_flow = htonl ((6 << 28) | (0 << 20) | 0);

  // Payload length (16 bits)
  // iphdr.ip6_plen is set for each fragment in loop below.

  // Next header (8 bits): 6 for TCP
  // We'll change this later, otherwise TCP checksum will be wrong.
  iphdr.ip6_nxt = IPPROTO_TCP;

  // Hop limit (8 bits): default to maximum value
  iphdr.ip6_hops = 255;

  // Source IPv6 address (128 bits)
  if ((status = inet_pton (AF_INET6, src_ip, &(iphdr.ip6_src))) != 1) {
    fprintf (stderr, "inet_pton() failed.\nError message: %s", strerror (status));
    exit (EXIT_FAILURE);
  }

  // Destination IPv6 address (128 bits)
  if ((status = inet_pton (AF_INET6, dst_ip, &(iphdr.ip6_dst))) != 1) {
    fprintf (stderr, "inet_pton() failed.\nError message: %s", strerror (status));
    exit (EXIT_FAILURE);
  }

  // TCP header

  // Source port number (16 bits)
  tcphdr.th_sport = htons (80);

  // Destination port number (16 bits)
  tcphdr.th_dport = htons (80);

  // Sequence number (32 bits)
  tcphdr.th_seq = htonl (0);

  // Acknowledgement number (32 bits): 0 in first packet of SYN/ACK process
  tcphdr.th_ack = htonl (0);

  // Reserved (4 bits): should be 0
  tcphdr.th_x2 = 0;

  // Data offset (4 bits): size of TCP header in 32-bit words
  tcphdr.th_off = TCP_HDRLEN / 4;

  // Flags (8 bits)

  // FIN flag (1 bit)
  tcp_flags[0] = 0;

  // SYN flag (1 bit): set to 1
  tcp_flags[1] = 1;

  // RST flag (1 bit)
  tcp_flags[2] = 0;

  // PSH flag (1 bit)
  tcp_flags[3] = 0;

  // ACK flag (1 bit)
  tcp_flags[4] = 0;

  // URG flag (1 bit)
  tcp_flags[5] = 0;

  // ECE flag (1 bit)
  tcp_flags[6] = 0;

  // CWR flag (1 bit)
  tcp_flags[7] = 0;

  tcphdr.th_flags = 0;
  for (i=0; i<8; i++) {
    tcphdr.th_flags += (tcp_flags[i] << i);
  }

  // Window size (16 bits)
  tcphdr.th_win = htons (65535);

  // Urgent pointer (16 bits): 0 (only valid if URG flag is set)
  tcphdr.th_urp = htons (0);

  // TCP checksum (16 bits)
  tcphdr.th_sum = tcp6_checksum (iphdr, tcphdr, payload, payloadlen);

  // Set our Next Header fields
  if (dst_nopt > 0) {
    dsthdr.nxt_hdr = IPPROTO_TCP;
  }

  if (hbh_nopt > 0) {
    iphdr.ip6_nxt = IPPROTO_HOPOPTS;
    if (nframes == 1)  {
      if (dst_nopt > 0) {
        hophdr.nxt_hdr = IPPROTO_DSTOPTS;
      } else {
        hophdr.nxt_hdr = IPPROTO_TCP;
      }
    } else {
      hophdr.nxt_hdr = IPPROTO_FRAGMENT;
    }

  } else {
    if (nframes == 1)  {
      if (dst_nopt > 0) {
        iphdr.ip6_nxt = IPPROTO_DSTOPTS;
      } else {
        iphdr.ip6_nxt = IPPROTO_TCP;
      }
    } else {
      iphdr.ip6_nxt = IPPROTO_FRAGMENT;
    }
  }

  // Build buffer array containing fragmentable portion.

  // Copy destination header and options to buffer, if specified.
  c = 0;  // Index of buffer
  indx = 0;  // Index is zero at start of destination header.
  if (dst_nopt > 0) {

    // Copy destination extension header (without options) to ethernet frame.
    memcpy (fragbuffer + c, &dsthdr, DST_HDRLEN * sizeof (uint8_t));
    c += DST_HDRLEN;
    indx += DST_HDRLEN;

    // Copy destination extension header options to ethernet frame.
    for (j=0; j<dst_nopt; j++) {
      // Pad as needed to achieve alignment requirements for option j (Section 4.2 of RFC 2460).
      option_pad (&indx, fragbuffer, &c, dst_x[j], dst_y[j]);

      // Copy destination option to ethernet frame.
      memcpy (fragbuffer + c, dst_options[j], dst_optlen[j] * sizeof (uint8_t));
      c += dst_optlen[j];
      indx += dst_optlen[j];
    }

    // Now pad last option to next 8-byte boundary (Section 4.2 of RFC 2460).
    option_pad (&indx, fragbuffer, &c, 8, 0);
  }
  // TCP header
  memcpy (fragbuffer + c, &tcphdr, TCP_HDRLEN * sizeof (uint8_t));
  c += TCP_HDRLEN;

  // TCP data
  memcpy (fragbuffer + c, payload, payloadlen * sizeof (uint8_t));
  c += payloadlen;

  // Submit request for a raw socket descriptor.
  if ((sd = socket (PF_PACKET, SOCK_RAW, htons (ETH_P_ALL))) < 0) {
    perror ("socket() failed ");
    exit (EXIT_FAILURE);
  }

  // Loop through fragments.
  for (i=0; i<nframes; i++) {

    // Set ethernet frame contents to zero initially.
    memset (ether_frame, 0, IP_MAXPACKET * sizeof (uint8_t));

    // Index of ethernet frame.
    c = 0;

    // Fill out ethernet frame header.

    // Copy destination and source MAC addresses to ethernet frame.
    memcpy (ether_frame, dst_mac, 6 * sizeof (uint8_t));
    memcpy (ether_frame + 6, src_mac, 6 * sizeof (uint8_t));

    // Next is ethernet type code (ETH_P_IPV6 for IPv6).
    // http://www.iana.org/assignments/ethernet-numbers
    ether_frame[12] = ETH_P_IPV6 / 256;
    ether_frame[13] = ETH_P_IPV6 % 256;
    c += ETH_HDRLEN;

    // Next is ethernet frame data

    // Payload length (16 bits): See 3 of RFC 2460.
    // Set to zero if hop-by-hop extension header includes a jumbogram.
    if (nframes == 1) {
      iphdr.ip6_plen = htons (hoplen + len[i]);
    } else {
      iphdr.ip6_plen = htons (hoplen + FRG_HDRLEN + len[i]);
    }

    // Copy IPv6 header to ethernet frame.
    memcpy (ether_frame + c, &iphdr, IP6_HDRLEN * sizeof (uint8_t));
    c += IP6_HDRLEN;

    // Add hop-by-hop header and options, if specified.
    indx = 0;  // Index is zero at start of hop-by-hop header.
    if (hbh_nopt > 0) {

      // Copy hop-by-hop extension header (without options) to ethernet frame.
      memcpy (ether_frame + c, &hophdr, HOP_HDRLEN * sizeof (uint8_t));
      c += HOP_HDRLEN;
      indx += HOP_HDRLEN;

      // Copy hop-by_hop extension header options to ethernet frame.
      for (j=0; j<hbh_nopt; j++) {
        // Pad as needed to achieve alignment requirements for option j (Section 4.2 of RFC 2460).
        option_pad (&indx, ether_frame, &c, hbh_x[j], hbh_y[j]);

        // Copy hop-by-hop option to ethernet frame.
        memcpy (ether_frame + c, hbh_options[j], hbh_optlen[j] * sizeof (uint8_t));
        c += hbh_optlen[j];
        indx += hbh_optlen[j];
      }

      // Now pad last option to next 8-byte boundary (Section 4.2 of RFC 2460).
      option_pad (&indx, ether_frame, &c, 8, 0);
    }

    // Fill out and copy fragmentation extension header, if necessary, to ethernet frame.
    if (nframes > 1) {
      if (dst_nopt > 0) {
        fraghdr.ip6f_nxt = IPPROTO_DSTOPTS;  // Destination extension header
      } else {
        fraghdr.ip6f_nxt = IPPROTO_TCP;  // Upper layer protocol
      }
      fraghdr.ip6f_reserved = 0;  // Reserved
      frag_flags[1] = 0;  // Reserved
      if (i < (nframes - 1)) {
        frag_flags[0] = 1;  // More fragments to follow
      } else {
        frag_flags[0] = 0;  // This is the last fragment
      }
      fraghdr.ip6f_offlg = htons ((offset[i] << 3) + frag_flags[0] + (frag_flags[1] <<1));
      fraghdr.ip6f_ident = htonl (31415);
      memcpy (ether_frame + c, &fraghdr, FRG_HDRLEN * sizeof (uint8_t));
      c += FRG_HDRLEN;
    }

    // Copy fragmentable portion of packet to ethernet frame.
    memcpy (ether_frame + c, fragbuffer + (offset[i] * 8), len[i] * sizeof (uint8_t));
    c += len[i];

    // Ethernet frame length
    frame_length = c;

    // Send ethernet frame to socket.
    printf ("Sending fragment: %i\n", i);
    if ((bytes = sendto (sd, ether_frame, frame_length, 0, (struct sockaddr *) &device, sizeof (device))) <= 0) {
      perror ("sendto() failed");
      exit (EXIT_FAILURE);
    }
  }

  // Close socket descriptor.
  close (sd);

  // Free allocated memory.
  free (src_mac);
  free (dst_mac);
  free (ether_frame);
  free (interface);
  free (target);
  free (src_ip);
  free (dst_ip);
  free (tcp_flags);
  free (payload);
  free (frag_flags);
  free (fragbuffer);
  free (hbh_optlen);
  for (i=0; i<MAX_HBHOPTIONS; i++) {
    free (hbh_options[i]);
  }
  free (hbh_options);
  free (hbh_x);
  free (hbh_y);
  free (dst_optlen);
  for (i=0; i<MAX_DSTOPTIONS; i++) {
    free (dst_options[i]);
  }
  free (dst_options);
  free (dst_x);
  free (dst_y);

  return (EXIT_SUCCESS);
}

// Checksum function
uint16_t
checksum (uint16_t *addr, int len)
{
  int nleft = len;
  int sum = 0;
  uint16_t *w = addr;
  uint16_t answer = 0;

  while (nleft > 1) {
    sum += *w++;
    nleft -= sizeof (uint16_t);
  }

  if (nleft == 1) {
    *(uint8_t *) (&answer) = *(uint8_t *) w;
    sum += answer;
  }

  sum = (sum >> 16) + (sum & 0xFFFF);
  sum += (sum >> 16);
  answer = ~sum;
  return (answer);
}

// Build IPv6 TCP pseudo-header and call checksum function (Section 8.1 of RFC 2460).
uint16_t
tcp6_checksum (struct ip6_hdr iphdr, struct tcphdr tcphdr, uint8_t *payload, int payloadlen)
{
  uint32_t lvalue;
  char buf[IP_MAXPACKET], cvalue;
  char *ptr;
  int chksumlen = 0;
  int i;

  memset (buf, 0, IP_MAXPACKET * sizeof (uint8_t));

  ptr = &buf[0];  // ptr points to beginning of buffer buf

  // Copy source IP address into buf (128 bits)
  memcpy (ptr, &iphdr.ip6_src.s6_addr, sizeof (iphdr.ip6_src.s6_addr));
  ptr += sizeof (iphdr.ip6_src.s6_addr);
  chksumlen += sizeof (iphdr.ip6_src.s6_addr);

  // Copy destination IP address into buf (128 bits)
  memcpy (ptr, &iphdr.ip6_dst.s6_addr, sizeof (iphdr.ip6_dst.s6_addr));
  ptr += sizeof (iphdr.ip6_dst.s6_addr);
  chksumlen += sizeof (iphdr.ip6_dst.s6_addr);

  // Copy TCP length to buf (32 bits)
  lvalue = htonl (sizeof (tcphdr) + payloadlen);
  memcpy (ptr, &lvalue, sizeof (lvalue));
  ptr += sizeof (lvalue);
  chksumlen += sizeof (lvalue);

  // Copy zero field to buf (24 bits)
  *ptr = 0; ptr++;
  *ptr = 0; ptr++;
  *ptr = 0; ptr++;
  chksumlen += 3;

  // Copy next header field to buf (8 bits)
  memcpy (ptr, &iphdr.ip6_nxt, sizeof (iphdr.ip6_nxt));
  ptr += sizeof (iphdr.ip6_nxt);
  chksumlen += sizeof (iphdr.ip6_nxt);

  // Copy TCP source port to buf (16 bits)
  memcpy (ptr, &tcphdr.th_sport, sizeof (tcphdr.th_sport));
  ptr += sizeof (tcphdr.th_sport);
  chksumlen += sizeof (tcphdr.th_sport);

  // Copy TCP destination port to buf (16 bits)
  memcpy (ptr, &tcphdr.th_dport, sizeof (tcphdr.th_dport));
  ptr += sizeof (tcphdr.th_dport);
  chksumlen += sizeof (tcphdr.th_dport);

  // Copy sequence number to buf (32 bits)
  memcpy (ptr, &tcphdr.th_seq, sizeof (tcphdr.th_seq));
  ptr += sizeof (tcphdr.th_seq);
  chksumlen += sizeof (tcphdr.th_seq);

  // Copy acknowledgement number to buf (32 bits)
  memcpy (ptr, &tcphdr.th_ack, sizeof (tcphdr.th_ack));
  ptr += sizeof (tcphdr.th_ack);
  chksumlen += sizeof (tcphdr.th_ack);

  // Copy data offset to buf (4 bits) and
  // copy reserved bits to buf (4 bits)
  cvalue = (tcphdr.th_off << 4) + tcphdr.th_x2;
  memcpy (ptr, &cvalue, sizeof (cvalue));
  ptr += sizeof (cvalue);
  chksumlen += sizeof (cvalue);

  // Copy TCP flags to buf (8 bits)
  memcpy (ptr, &tcphdr.th_flags, sizeof (tcphdr.th_flags));
  ptr += sizeof (tcphdr.th_flags);
  chksumlen += sizeof (tcphdr.th_flags);

  // Copy TCP window size to buf (16 bits)
  memcpy (ptr, &tcphdr.th_win, sizeof (tcphdr.th_win));
  ptr += sizeof (tcphdr.th_win);
  chksumlen += sizeof (tcphdr.th_win);

  // Copy TCP checksum to buf (16 bits)
  // Zero, since we don't know it yet
  *ptr = 0; ptr++;
  *ptr = 0; ptr++;
  chksumlen += 2;

  // Copy urgent pointer to buf (16 bits)
  memcpy (ptr, &tcphdr.th_urp, sizeof (tcphdr.th_urp));
  ptr += sizeof (tcphdr.th_urp);
  chksumlen += sizeof (tcphdr.th_urp);

  // Copy payload to buf
  memcpy (ptr, payload, payloadlen * sizeof (uint8_t));
  ptr += payloadlen;
  chksumlen += payloadlen;

  // Pad to the next 16-bit boundary
  i = 0;
  while (((payloadlen+i)%2) != 0) {
    i++;
    chksumlen++;
    ptr++;
  }

  return checksum ((uint16_t *) buf, chksumlen);
}

// Provide padding as needed to achieve alignment requirements of hop-by-hop or destination option.
int
option_pad (int *indx, uint8_t *padding, int *c, int x, int y)
{
  int needpad;

  // Find number of padding bytes needed to achieve alignment requirements for option (Section 4.2 of RFC 2460).
  // Alignment is expressed as xN + y, which means the start of the option must occur at xN + y bytes
  // from the start of the hop-by-hop or destination header, where N is integer 0, 1, 2, ...etc.
  needpad = 0;
  while (((*indx + needpad) % x) != y) {
    needpad++;
  }

  // If required padding = 1 byte, we use Pad1 option.
  if (needpad == 1) {
    padding[*c] = 0;  // Padding option type: Pad1
    (*indx)++;
    (*c)++;

  // If required padding is > 1 byte, we use PadN option.
  } else if (needpad > 1) {
    padding[*c] = 1;  // Padding option type: PadN
    (*indx)++;
    (*c)++;
    padding[*c] = needpad - 2;  // PadN length: N - 2
    (*indx)++;
    (*c)++;
    memset (padding + (*c), 0, (needpad - 2) * sizeof (uint8_t));
    (*indx) += needpad - 2;
    (*c) += needpad - 2;
  }

  return (EXIT_SUCCESS);
}

// Allocate memory for an array of chars.
char *
allocate_strmem (int len)
{
  void *tmp;

  if (len <= 0) {
    fprintf (stderr, "ERROR: Cannot allocate memory because len = %i in allocate_strmem().\n", len);
    exit (EXIT_FAILURE);
  }

  tmp = (char *) malloc (len * sizeof (char));
  if (tmp != NULL) {
    memset (tmp, 0, len * sizeof (char));
    return (tmp);
  } else {
    fprintf (stderr, "ERROR: Cannot allocate memory for array allocate_strmem().\n");
    exit (EXIT_FAILURE);
  }
}

// Allocate memory for an array of unsigned chars.
uint8_t *
allocate_ustrmem (int len)
{
  void *tmp;

  if (len <= 0) {
    fprintf (stderr, "ERROR: Cannot allocate memory because len = %i in allocate_ustrmem().\n", len);
    exit (EXIT_FAILURE);
  }

  tmp = (uint8_t *) malloc (len * sizeof (uint8_t));
  if (tmp != NULL) {
    memset (tmp, 0, len * sizeof (uint8_t));
    return (tmp);
  } else {
    fprintf (stderr, "ERROR: Cannot allocate memory for array allocate_ustrmem().\n");
    exit (EXIT_FAILURE);
  }
}

// Allocate memory for an array of pointers to arrays of unsigned chars.
uint8_t **
allocate_ustrmemp (int len)
{
  void *tmp;

  if (len <= 0) {
    fprintf (stderr, "ERROR: Cannot allocate memory because len = %i in allocate_ustrmemp().\n", len);
    exit (EXIT_FAILURE);
  }

  tmp = (uint8_t **) malloc (len * sizeof (uint8_t *));
  if (tmp != NULL) {
    memset (tmp, 0, len * sizeof (uint8_t *));
    return (tmp);
  } else {
    fprintf (stderr, "ERROR: Cannot allocate memory for array allocate_ustrmemp().\n");
    exit (EXIT_FAILURE);
  }
}

// Allocate memory for an array of ints.
int *
allocate_intmem (int len)
{
  void *tmp;

  if (len <= 0) {
    fprintf (stderr, "ERROR: Cannot allocate memory because len = %i in allocate_intmem().\n", len);
    exit (EXIT_FAILURE);
  }

  tmp = (int *) malloc (len * sizeof (int));
  if (tmp != NULL) {
    memset (tmp, 0, len * sizeof (int));
    return (tmp);
  } else {
    fprintf (stderr, "ERROR: Cannot allocate memory for array allocate_intmem().\n");
    exit (EXIT_FAILURE);
  }
}

/*
 * SSToPer, Linux SSTP Client
 * Christophe Alladoum < christophe __DOT__ alladoum __AT__ hsc __DOT__ fr>
 * Herve Schauer Consultants (http://www.hsc.fr)
 *
 *            GNU GENERAL PUBLIC LICENSE
 *              Version 2, June 1991
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (
 * at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <stdint.h>
#include <sys/time.h>

#ifdef __GNUC__
#define UNUSED __attribute__ ((unused))
#else
#define UNUSED
#endif

#define TRUE 1
#define FALSE 0

#define MAX_LINE_LENGTH 128

/* System properties */
#define __UNSIGNED_LONG_LONG_MAX__ (~0ULL)

/* SSTP Properties */
#define SSTP_HTTPS_RESOURCE "/sra_{BA195980-CD49-458b-9E23-C84EE0ADCD75}/"
#define SSTP_VERSION 0x10
#define SSTP_MIN_LEN 4
#define SSTP_MAX_ATTR 256
#define SSTP_NEGOCIATION_TIMER 60
#define SSTP_PING_TIMER 30
#define SSTP_MAX_INIT_RETRY 5
#define SSTP_SEED_PREFIX "SSTP inner method derived CMK"
#define SSTP_CMAC_SEED_PREFIX_LEN  29
#define SHA1_HASH_LEN 0x0014
#define SHA256_HASH_LEN 0x0020

#define PPP_MAX_MTU 4096
#define PPP_MAX_MRU 4096

#define NO_PRIV_USER "nobody"
#define NO_PRIV_DIR "/tmp/sstoper-XXXXXX"

/* SSTP Packet Type */
enum packet_types
  {
    SSTP_DATA_PACKET = 0x00,
    SSTP_CONTROL_PACKET = 0x01
  };


/* SSTP Protocol Type */
enum sstp_encapsulated_protocol_types
  {
    SSTP_ENCAPSULATED_PROTOCOL_PPP = 0x0001
  };


/* SSTP Status Message */
enum control_messages_types
  {
    SSTP_MSG_CALL_CONNECT_REQUEST = 0x0001,
    SSTP_MSG_CALL_CONNECT_ACK = 0x0002,
    SSTP_MSG_CALL_CONNECT_NAK = 0x0003,
    SSTP_MSG_CALL_CONNECTED = 0x0004,
    SSTP_MSG_CALL_ABORT = 0x0005,
    SSTP_MSG_CALL_DISCONNECT = 0x0006,
    SSTP_MSG_CALL_DISCONNECT_ACK = 0x0007,
    SSTP_MSG_ECHO_REQUEST = 0x0008,
    SSTP_MSG_ECHO_REPONSE = 0x0009
  };
const static UNUSED char* control_messages_types_str[] =
  {"",
   "SSTP_MSG_CALL_CONNECT_REQUEST",
   "SSTP_MSG_CALL_CONNECT_ACK",
   "SSTP_MSG_CALL_CONNECT_NAK",
   "SSTP_MSG_CALL_CONNECTED",
   "SSTP_MSG_CALL_ABORT",
   "SSTP_MSG_CALL_DISCONNECT",
   "SSTP_MSG_CALL_DISCONNECT_ACK",
   "SSTP_MSG_ECHO_REQUEST",
   "SSTP_MSG_ECHO_REPONSE",
  };


/* SSTP Attribute Message Type */
enum attr_types
  {
    SSTP_ATTRIB_NO_ERROR = 0x00,
    SSTP_ATTRIB_ENCAPSULATED_PROTOCOL_ID = 0x01,
    SSTP_ATTRIB_STATUS_INFO = 0x02,
    SSTP_ATTRIB_CRYPTO_BINDING = 0x03,
    SSTP_ATTRIB_CRYPTO_BINDING_REQ = 0x04
  };
const static UNUSED char* attr_types_str[] =
  {
    "SSTP_ATTRIB_NO_ERROR",
    "SSTP_ATTRIB_ENCAPSULATED_PROTOCOL_ID",
    "SSTP_ATTRIB_STATUS_INFO",
    "SSTP_ATTRIB_CRYPTO_BINDING",
    "SSTP_ATTRIB_CRYPTO_BINDING_REQ",
  };


/* Crypto Binding Request Attribute */
enum crypto_req_attrs
  {
    CERT_HASH_PROTOCOL_SHA1 = 0x01,
    CERT_HASH_PROTOCOL_SHA256 = 0x02
  };
static UNUSED char* crypto_req_attrs_str[]=
  {
    "",
    "CERT_HASH_PROTOCOL_SHA1",
    "CERT_HASH_PROTOCOL_SHA256"
  };


/* Status Info Attribute */
enum attr_status
  {
    ATTRIB_STATUS_NO_ERROR = 0x00000000,
    ATTRIB_STATUS_DUPLICATE_ATTRIBUTE = 0x00000001,
    ATTRIB_STATUS_UNRECOGNIZED_ATTRIBUTE = 0x00000002,
    ATTRIB_STATUS_INVALID_ATTRIB_VALUE_LENGTH = 0x00000003,
    ATTRIB_STATUS_VALUE_NOT_SUPPORTED = 0x00000004,
    ATTRIB_STATUS_UNACCEPTED_FRAME_RECEIVED = 0x00000005,
    ATTRIB_STATUS_RETRY_COUNT_EXCEEDED = 0x00000006,
    ATTRIB_STATUS_INVALID_FRAME_RECEIVED = 0x00000007,
    ATTRIB_STATUS_NEGOTIATION_TIMEOUT = 0x00000008,
    ATTRIB_STATUS_ATTRIB_NOT_SUPPORTED_IN_MSG = 0x00000009,
    ATTRIB_STATUS_REQUIRED_ATTRIBUTE_MISSING = 0x0000000a,
    ATTRIB_STATUS_STATUS_INFO_NOT_SUPPORTED_IN_MSG = 0x0000000b
  };
const static UNUSED char* attrib_status_str[] =
  {
    "ATTRIB_STATUS_NO_ERROR",
    "ATTRIB_STATUS_DUPLICATE_ATTRIBUTE",
    "ATTRIB_STATUS_UNRECOGNIZED_ATTRIBUTE",
    "ATTRIB_STATUS_INVALID_ATTRIB_VALUE_LENGTH",
    "ATTRIB_STATUS_VALUE_NOT_SUPPORTED",
    "ATTRIB_STATUS_UNACCEPTED_FRAME_RECEIVED",
    "ATTRIB_STATUS_RETRY_COUNT_EXCEEDED",
    "ATTRIB_STATUS_INVALID_FRAME_RECEIVED",
    "ATTRIB_STATUS_NEGOTIATION_TIMEOUT",
    "ATTRIB_STATUS_ATTRIB_NOT_SUPPORTED_IN_MSG",
    "ATTRIB_STATUS_REQUIRED_ATTRIBUTE_MISSING",
    "ATTRIB_STATUS_STATUS_INFO_NOT_SUPPORTED_IN_MSG",
  };


/* sstp client status */
enum client_status
  {
    CLIENT_CALL_DISCONNECTED,
    CLIENT_CONNECT_REQUEST_SENT,
    CLIENT_CONNECT_ACK_RECEIVED,
    CLIENT_CALL_CONNECTED
  };
const static UNUSED char* client_status_str[] =
  {
    "CLIENT_CALL_DISCONNECTED",
    "CLIENT_CONNECT_REQUEST_SENT",
    "CLIENT_CONNECT_ACK_RECEIVED",
    "CLIENT_CALL_CONNECTED",
  };


/* data structures */
typedef struct __sstp_header
{
  uint8_t version;
  uint8_t reserved;
  uint16_t length;
} sstp_header_t;

typedef struct __sstp_control_header
{
  uint16_t message_type;
  uint16_t num_attributes;
} sstp_control_header_t;

typedef struct __sstp_attribute_header
{
  uint8_t reserved;
  uint8_t attribute_id;
  uint16_t packet_length;
} sstp_attribute_header_t;

/* attribute structures */
typedef struct __sstp_attribute
{
  uint16_t length;
  void *data;
} sstp_attribute_t;

/* uint24_t n'existe pas */
#ifndef uint24_t
typedef struct _uint24_t
{
  uint8_t byte[3];
} uint24_t;
#endif

typedef struct __sstp_attribute_crypto_bind_req
{
  uint24_t reserved1;
  uint8_t hash_bitmask;
  uint32_t nonce[8];
} sstp_attribute_crypto_bind_req_t;

typedef struct __sstp_attribute_crypto_bind
{
  uint24_t reserved1;
  uint8_t hash_bitmask;
  uint32_t nonce[8];
  uint32_t certhash[8];
  uint32_t cmac[8];
} sstp_attribute_crypto_bind_t;


typedef struct __sstp_attribute_status_info
{
  uint24_t reserved1;
  uint8_t attrib_id;
  uint32_t status;
} sstp_attribute_status_info_t;

enum _flags
  {
    REMOTE_DISCONNECTION = 0x1,
    NEGOCIATION_TIMER_RAISED = 0x2,
    HELLO_TIMER_RAISED = 0x4,
  };

/* sstp client context */
typedef struct __sstp_context
{
  unsigned char state;
  unsigned char flags;
  unsigned char retry;
  pid_t pppd_pid;
  struct timeval negociation_timer;
  struct timeval hello_timer;
  uint8_t hash_algorithm;
  uint32_t nonce[8];
  uint32_t certhash[8];
  uint32_t cmk[8];
  uint32_t cmac[8];
} sstp_context_t;

sstp_context_t* ctx;


typedef struct __sstp_session
{
  unsigned long rx_bytes;
  unsigned long tx_bytes;
  struct timeval tv_start;
  struct timeval tv_end;
} sstp_session_t;

sstp_session_t* sess;


typedef struct __chap_context
{
  unsigned char response_challenge[16];
  unsigned char response_reserved[8];
  unsigned char response_nt_response[24];
  unsigned char response_flags[1];
} chap_context_t;

chap_context_t* chap_ctx;

/* functions declarations  */
void set_client_status(uint8_t status);
int https_session_negociation();
void sstp_loop(pid_t);
int sstp_fork();
int sstp_decode(void* rbuffer, ssize_t sstp_length);


/* crypto functions */
uint8_t* sstp_hmac(unsigned char* key, unsigned char* d, uint16_t n);
void NtPasswordHash(uint8_t *password_hash, const uint8_t *password, size_t password_len);
void HashNtPasswordHash(uint8_t *password_hash_hash, const uint8_t *password_hash);
void GetMasterKey(void* MasterKey, void* PasswordHashHash, void* NTResponse);
void GetAsymmetricStartKey(void* MasterSessionKey, void* MasterKey,
			   uint8_t KeyLength, uint8_t IsSend, uint8_t IsServer);

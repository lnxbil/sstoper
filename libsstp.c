#define _POSIX_SOURCE 1

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <openssl/sha.h>
#include <openssl/md4.h>
#include <openssl/hmac.h>
#include <openssl/md4.h>
#include <gnutls/x509.h>
#include <gnutls/gnutls.h>

#include "libsstp.h"
#include "sstpclient.h"


#if defined ___Linux___
#include <pty.h>

#elif defined ___Darwin___
/*  Darwin compat */
#include <util.h>

#elif defined ___FreeBSD___
/* FreeBSD compat */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <libutil.h>

#elif defined ___OpenBSD___
/* OpenBSD compat */
#include <termios.h>
#include <util.h>

#endif 



/**
 * Generate an GUID identifier for the SSTP connection
 *
 * @param data : buffer to store guid
 */
void generate_guid(char data[])
{
  uint32_t data1, data4;
  uint16_t data2, data3;
  struct timeval tv;
  unsigned int seed;
  
  gettimeofday(&tv, NULL);
  seed = tv.tv_usec * tv.tv_sec;
  seed ^= getpid();
      
  memset(data, 0, 39);
  srand (seed);
  data1 = (rand() + 1) * (sizeof(uint32_t) * 8);
  data2 = (rand() + 1) * (sizeof(uint16_t) * 8);
  data3 = (rand() + 1) * (sizeof(uint16_t) * 8);
  data4 = (rand() + 1) * (sizeof(uint32_t) * 8);
  snprintf(data, 38, "{%.4X-%.2X-%.2X-%.4X}", data1, data2, data3, data4);

  if (cfg->verbose) xlog(LOG_INFO, "Using GUID %s\n", data);
}


/**
 * Change client state
 *
 * @param status : new status
 */
void set_client_status(uint8_t status)
{
  ctx->state = status;

  if (cfg->verbose)
    xlog(LOG_INFO, "Client status : %s (%#x)\n",
	 client_status_str[ctx->state], ctx->state);
}


/**
 * Header validation.
 *
 * @param header : sstp header to analyse
 * @param recv_len : number of bytes received
 * @return TRUE if valid, FALSE otherwise
 */
int is_valid_header(sstp_header_t* header, ssize_t recv_len)
{
  
  if (header->version != SSTP_VERSION)
    {
      if (cfg->verbose)
	xlog(LOG_ERROR, "Invalid version (%#x)", header->version);
      return FALSE;
    }

  if (header->reserved != SSTP_DATA_PACKET &&
      header->reserved != SSTP_CONTROL_PACKET)
    {
      if (cfg->verbose)
	xlog(LOG_ERROR, "Invalid packet type (%#x)\n", header->reserved);
      return FALSE;
    }

  /*
   * note : bug server sstp ou ppp
   * le 1er packet ppp recu du serveur possede une taille de paquet differente de celle
   * annonce dans le header sstp -> test de la taille echoue 
   */
  if (ntohs(header->length) != recv_len)
    {
      if (cfg->verbose > 2)
	xlog(LOG_DEBUG, "Unmatching length: annonced %lu, received %lu\n",
	     ntohs(header->length), recv_len);
      return FALSE;
      }

  return TRUE;
}


/**
 * Check whether received packet has Control flag raised.
 *
 * @param packet_header : received SSTP header
 * @return TRUE if valid, FALSE otherwise
 */
int is_control_packet(sstp_header_t* packet_header)
{
  return (packet_header->reserved == SSTP_CONTROL_PACKET) ? TRUE : FALSE;
}


/**
 * Send HTTP request to start a new SSTP connection.
 *
 * @return 0 if all good, negative value otherwise
 */
int https_session_negociation()
{
  ssize_t rbytes;
  char buf[1024], guid[39];

  /* sending HTTP request */
  rbytes = -1;
  memset(guid, 0, 39);
  memset(buf, 0, 1024);
  generate_guid(guid);
  
  rbytes = snprintf(buf, 1024,
		    "SSTP_DUPLEX_POST %s HTTP/1.1\r\n"
		    "Host: %s\r\n" /* <-- note: le hostname jamais valide cote serveur */
		    "SSTPCORRELATIONID: nawak%s\r\n" /* <-- note: on peut mettre nawak ici */
		    "Content-Length: %llu\r\n"
		    "Cookie: ClientHTTPCookie=True; ClientBypassHLAuth=True\r\n"
		    "\r\n",
		    SSTP_HTTPS_RESOURCE,
		    cfg->server,
		    guid,
		    __UNSIGNED_LONG_LONG_MAX__);

  if (cfg->verbose > 2)
    xlog(LOG_DEBUG, "Sending: %lu bytes\n%s\n", rbytes, buf);

  sstp_send(buf, rbytes);

  /* waiting for and parsing response */
  memset(buf, 0, 1024);
  rbytes = gnutls_record_recv (tls, buf, 1024);
      
  if (rbytes > 0)
    {
      xlog(LOG_INFO , "<-- %lu bytes\n", rbytes);

      if (cfg->verbose > 2)
	xlog(LOG_DEBUG , "Received: %s\n", buf);
    }
  else if (rbytes == 0)
    {
      xlog(LOG_INFO , "Connection closed by beer.\n");
      return -1;
    }
  else 
    {
      xlog(LOG_ERROR, "gnutls_record_recv: %s\n", gnutls_strerror(rbytes));
      return -2;
    }

  if (strncmp(buf, "HTTP/1.1 200", 12)) 
    return -3;

  return 0;
}

/**
 * Emits SSTP negociation request, ie Control message with ENCAPSULATED_PROTOCOL_ID
 * attribute. Negociation timer is set up and state is changed.
 */
void sstp_init()
{
  uint16_t attribute_data;
  void* attribute;
  size_t attribute_len;

  attribute_data = htons(SSTP_ENCAPSULATED_PROTOCOL_PPP);
  attribute_len = sizeof(sstp_attribute_header_t) + sizeof(uint16_t);
  attribute = create_attribute(SSTP_ATTRIB_ENCAPSULATED_PROTOCOL_ID,
			       (void*)&attribute_data, sizeof(uint16_t));

  send_sstp_control_packet(SSTP_MSG_CALL_CONNECT_REQUEST, attribute,
			   1, attribute_len);

  free(attribute);

  alarm(ctx->negociation_timer.tv_sec);
  set_client_status(CLIENT_CONNECT_REQUEST_SENT);

}


/**
 * The main loop will be called right after the end of HTTPS negociation and
 * - allocates SSTP client context regions
 * - start an SSTP negociation
 * - handle receive packets
 * - send packets
 */
void sstp_loop()
{
  size_t read_max_size;
  fd_set rcv_fd;
  int retcode;

  /* set buffer max len receive */
  read_max_size = gnutls_record_get_max_size(tls);
  
  /* initialize sstp context */
  ctx = (sstp_context_t*) xmalloc(sizeof(sstp_context_t));
  ctx->retry = 5;
  ctx->state = CLIENT_CALL_DISCONNECTED;
  ctx->negociation_timer.tv_sec = SSTP_NEGOCIATION_TIMER;
  ctx->hello_timer.tv_sec = SSTP_NEGOCIATION_TIMER;
  ctx->pppd_pid = -1;

  /* initialize chap context */
  chap_ctx = (chap_context_t*) xmalloc(sizeof(chap_context_t));

  /* start sstp negociation */
  sstp_init();

  
  /* main loop */
  while(ctx->state != CLIENT_CALL_DISCONNECTED)
    {
      FD_ZERO(&rcv_fd);
      
      if (ctx->pppd_pid > 0)
	FD_SET(0, &rcv_fd);
      
      FD_SET(sockfd, &rcv_fd);   

      retcode = select(sockfd + 1, &rcv_fd, NULL, NULL, NULL);
      
      if ( retcode < 0 )
	{
	  xlog(LOG_ERROR, "sstp_loop: %s\n", strerror(errno));	  
	  set_client_status(CLIENT_CALL_DISCONNECTED);
	  break;
	}

      if (ctx->pppd_pid > 0 && FD_ISSET(0, &rcv_fd)) 
	{
	  /* read from 0 and sstp_send to dest */
	  char rbuffer[read_max_size];
	  ssize_t rbytes = -1;
	  rbytes = read(0, rbuffer, read_max_size);
	  if (rbytes > 0)
	    send_sstp_data_packet(rbuffer, rbytes);
	}
      
      if (FD_ISSET(sockfd, &rcv_fd)) 
	{
	  /* sstp_read data from sockfd and write it to 1 */
	  char rbuffer[read_max_size];
	  ssize_t rbytes;

	  rbytes = gnutls_record_recv (tls, rbuffer, read_max_size);
	  if (rbytes < 0)
	    {
	      retcode = rbytes;
	      xlog(LOG_ERROR, "sstp_loop: gnutls_record_recv: %s\n", gnutls_strerror(rbytes));
	    }
	  
	  else if (rbytes == 0) 
	    {
	      xlog(LOG_INFO, "sstp_loop: EOF\n");
	    }
	  
	  else 
	    {
	      xlog(LOG_INFO,"<--  %lu bytes\n", rbytes);
	      retcode = sstp_decode(rbuffer, rbytes);
	    }
	  
	  if (retcode < 0)
	    break;
	}

    }

  if (ctx->pppd_pid > 0) 
    {	  
      xlog(LOG_INFO, "Waiting for %s process (PID:%d) to end ... \t",
	   cfg->pppd_path, ctx->pppd_pid);
      
      kill(ctx->pppd_pid, SIGINT);
      waitpid(ctx->pppd_pid, &retcode, 0);
      
      if (retcode == 5)
	xlog(LOG_INFO, "[OK]\n");
      else
	xlog(LOG_INFO, "[K0] (%d)\n", retcode);

    }
      
  if (cfg->verbose)
    xlog(LOG_INFO, "Sending SSTP_MSG_CALL_DISCONNECT message.\n");
  
  send_sstp_control_packet(SSTP_MSG_CALL_DISCONNECT, NULL, 0, 0);
  
  free(chap_ctx);
  free(ctx);
}


/**
 * Decodes SSTP packet, checks first its header. If control packet, parse it and load
 * eventual attribute function
 *
 * @param rbuffer : buffer received from SSTP server
 * @param sstp_length : buffer length
 * @return 0 if decoding was successful, negative value otherwise. A special case was done
 * for invalid header since there seems to be a problem with server packet length. In this
 * case, received packet is just dropped.
 */
int sstp_decode(void* rbuffer, ssize_t sstp_length)
{
  sstp_header_t* sstp_header;
  int is_control, retcode;

  /* packet validation */
  sstp_header = (sstp_header_t*) rbuffer;
  if (!is_valid_header(sstp_header, sstp_length))
    {
      xlog(LOG_WARNING, "SSTP packet has invalid header. Dropped\n");
      return 0;
    } 

  /* determine sstp packet type */
  is_control = is_control_packet(sstp_header);
  
  if (cfg->verbose)
    xlog(LOG_INFO, "\t-> %s packet\n", is_control ? "Control" : "Data");

  sstp_length -= sizeof(sstp_header_t);
  if (sstp_length <= 0)
    {
      xlog(LOG_ERROR, "SSTP packet has incorrect length.\n");
      return -1;
    }
  
  
  if (is_control)
    {
      sstp_control_header_t* control_header;
      uint16_t control_type, control_num_attributes;
      void* attribute_ptr;

      
      control_header = (sstp_control_header_t*) (rbuffer + sizeof(sstp_header_t));
      control_type = ntohs( control_header->message_type );
      control_num_attributes = ntohs( control_header->num_attributes );
      attribute_ptr = (void*)(control_header) + sizeof(sstp_control_header_t);

      
      /* checking control header and control type */
      sstp_length -= sizeof(sstp_control_header_t);
      if (sstp_length < 0)
	{
	  xlog(LOG_ERROR, "SSTP control packet has invalid size\n");
	  return -1;
	}

       if (!control_type || control_type > SSTP_MSG_ECHO_REPONSE)
	{
	  xlog(LOG_ERROR, "Incorrect control packet\n");
	  return -1;  
	}
      
      
      /* parsing control header */
      if (cfg->verbose)
	{
	  xlog(LOG_INFO, "\t-> type: %s (%#.2x)\n",
	       control_messages_types_str[control_type], control_type);
	  xlog(LOG_INFO, "\t-> attribute number: %d\n", control_num_attributes);
	  xlog(LOG_INFO, "\t-> length: %d\n", (sstp_header->length - sizeof(sstp_header_t)));
	}
      
      switch (control_type)
	{
	case SSTP_MSG_CALL_CONNECT_ACK:
	  retcode = sstp_decode_attributes(control_num_attributes, attribute_ptr, sstp_length);
	  if (retcode < 0) return -1;

	  retcode = sstp_fork();
	  if (retcode < 0) return -1;
	  
	  ctx->pppd_pid = retcode;
	  if (cfg->verbose)
	    xlog (LOG_INFO, "%s forked as %d\n", cfg->pppd_path, ctx->pppd_pid);

	  break;
	    
	case SSTP_MSG_CALL_CONNECT_NAK:
	  if ( ctx->state==CLIENT_CONNECT_REQUEST_SENT ) return -1;
	  
	  retcode = sstp_decode_attributes(control_num_attributes, attribute_ptr, sstp_length);
	  if (retcode < 0) return -1;

	  if ( ctx->retry ) 
	    {
	      if (cfg->verbose) xlog(LOG_INFO, "Retrying ... (%d/%d)\n",
				     5-ctx->retry, 5);
	      ctx->negociation_timer.tv_sec = SSTP_NEGOCIATION_TIMER;
	      ctx->retry--;
	      sstp_init();
	    }

	  break;
	  
	case SSTP_MSG_CALL_ABORT:
	  retcode = sstp_decode_attributes(control_num_attributes, attribute_ptr, sstp_length);
	  if (retcode < 0) return -1;
	  break;
	  
	case SSTP_MSG_CALL_DISCONNECT:
	  retcode = sstp_decode_attributes(control_num_attributes, attribute_ptr, sstp_length);
	  if (retcode < 0) return -1;

	  send_sstp_control_packet(SSTP_MSG_CALL_DISCONNECT_ACK, NULL, 0, 0);
	  ctx->state = CLIENT_CALL_DISCONNECTED;
	  break;

	case SSTP_MSG_ECHO_REQUEST:
	  if (ctx->state != CLIENT_CALL_CONNECTED) return -1;
	  send_sstp_control_packet(SSTP_MSG_ECHO_REPONSE, NULL, 0, 0);
	  break;
	  
	case SSTP_MSG_ECHO_REPONSE:
	  if (ctx->state != CLIENT_CALL_CONNECTED) return -1;
	  alarm(0);
	  break;
	  
	  /*
	   * Client SHOULD NEVER receive teh following message.
	   * If so, close (dirtiliy) the client.
	   */
	case SSTP_MSG_CALL_CONNECT_REQUEST:
	case SSTP_MSG_CALL_DISCONNECT_ACK:
	default :
	  xlog(LOG_ERROR, "Client cannot handle type %#x\n", control_type);
	  ctx->state = CLIENT_CALL_DISCONNECTED;
	  return -1;
	}
      
    }
  else 
    {
      void* data_ptr;
      data_ptr = rbuffer + sizeof(sstp_header_t);

      /*
       * On intercepting a PPP success message, sstpclient will also send
       * a SSTP_MSG_CALL_CONNECTED message, allowing PPP data to be treated 
       * on server side.
       * See also : http://tools.ietf.org/search/rfc2759#section-4
       */
      
      if ( ntohs(*((uint16_t*)data_ptr)) == 0xc223 )
	{
	  uint8_t chap_handshake_code = *(uint8_t*)(data_ptr + 2);

	  /* if Success on PPP-CHAP */	
	  if (chap_handshake_code == 0x03 )
	    {
	      size_t attribute_len;
	      void* attribute;
	      sstp_attribute_crypto_bind_t crypto_settings;

	      /* compute cmac */
	      if (crypto_set_cmac() < 0)
		return -1;
	      
	      memset(&crypto_settings, 0, sizeof(sstp_attribute_crypto_bind_t));
	      
	      /* send SSTP_MSG_CALL_CONNECTED */
	      attribute_len = sizeof(sstp_attribute_header_t) + sizeof(sstp_attribute_crypto_bind_t);
	      crypto_settings.hash_bitmask = ctx->hash_algorithm;
	      memcpy(crypto_settings.nonce, ctx->nonce, sizeof(uint32_t)*8);
	      memcpy(crypto_settings.certhash, ctx->certhash, sizeof(uint32_t)*8);
	      memcpy(crypto_settings.cmac, ctx->cmac, sizeof(uint32_t)*8);
	      
	      attribute = create_attribute(SSTP_ATTRIB_CRYPTO_BINDING, &crypto_settings,
					   sizeof(sstp_attribute_crypto_bind_t));
	      
	      send_sstp_control_packet(SSTP_MSG_CALL_CONNECTED, attribute, 1, attribute_len);
	      
	      free(attribute);
	      
	      /* and set hello timer */
	      alarm(ctx->hello_timer.tv_sec);
	      set_client_status(CLIENT_CALL_CONNECTED);

	      /* send an sstp ping, response will stop the alarm */
	      send_sstp_control_packet(SSTP_MSG_ECHO_REQUEST, NULL, 0, 0);
	    }
	}
      
      retcode = write(1, data_ptr, sstp_length);

      if (retcode < 0) 
	{
	  xlog(LOG_ERROR, "write: %s\n", strerror(retcode));
	  return -1;
	}
    }

  return 0;
}
 

/**
 * Decode and parse every attribute provided within an SSTP control packet.
 *
 * @param attrnum : number of attributes
 * @param data : pointer to the beginning of attributes buffer
 * @param bytes_to_read : `data` length
 * @return 0 if all good, negative value otherwise
 */
int sstp_decode_attributes(uint16_t attrnum, void* data, ssize_t bytes_to_read) 
{
  void* attr_ptr;
  int retcode;
  
  attr_ptr = data;
  retcode = 0;

  
  /* attributes parsing */
  while (attrnum) 
    {
      sstp_attribute_header_t* attribute_header;
      void* attribute_data;
      uint8_t attribute_id;
      uint16_t attribute_length;

      if (bytes_to_read < sizeof(sstp_attribute_header_t)) 
	{
	  xlog(LOG_ERROR, "Incorrect attribute received. Leaving...\n");
	  return -1;
	}
      
      attribute_header = (sstp_attribute_header_t*) attr_ptr;
      attribute_id = attribute_header->attribute_id;
      attribute_length = ntohs( attribute_header->packet_length );
      attribute_data = (attr_ptr + sizeof(sstp_attribute_header_t));


      /* checking attribute header*/
      bytes_to_read -= attribute_length;
      if (bytes_to_read < 0) 
	{
	  xlog(LOG_ERROR, "Incorrect offset attempt. Leaving...\n");
	  return -1;
	}
      if (attribute_id > SSTP_ATTRIB_CRYPTO_BINDING_REQ)
	{
	  xlog(LOG_ERROR, "Incorrect attribute id.\n");
	  return -1;
	}
      
      /* parsing attribute header */
      if (cfg->verbose)
	{
	  xlog(LOG_INFO, "\t\t--> attr_id\t%s (%#.2x)\n",attr_types_str[attribute_id], attribute_id);
	  xlog(LOG_INFO, "\t\t--> len\t\t%d bytes\n", attribute_length);
	}

      switch (attribute_id)
	{
	case SSTP_ATTRIB_NO_ERROR:
	  break;

	case SSTP_ATTRIB_STATUS_INFO:
	  retcode = attribute_status_info(attribute_data, attribute_length);
	  break;
	  
	case SSTP_ATTRIB_CRYPTO_BINDING_REQ:	  
	  retcode = crypto_set_binding(attribute_data);
	  break;

	  /* case not to be treated on client side, ignoring */
	case SSTP_ATTRIB_CRYPTO_BINDING:
	case SSTP_ATTRIB_ENCAPSULATED_PROTOCOL_ID:
	default:
	  xlog(LOG_ERROR, "Attribute ID %#x is not handled on client side.\n", attribute_id);
	  retcode = -1;
	}

      if (retcode < 0) break;

      /* shifting to next attribute */
      attr_ptr += attribute_length;
      attrnum--;
    }
  
  return retcode;
}


/**
 * Generic function to send SSTP data to the server.
 *
 * @param data : buffer to be sent
 * @param data_length : `data` length
 */
void sstp_send(void* data, size_t data_length)
{
  ssize_t sbytes;

  sbytes = gnutls_record_send(tls, data, data_length);
  
  if (sbytes < 0)
    {
      xlog(LOG_ERROR, "gnutls_record_send: %s\n", gnutls_strerror(sbytes));
      exit(sbytes);
    }

  xlog(LOG_INFO, " --> %lu bytes\n", sbytes);
  
}


/**
 * Encapsulated data provided as argument inside a SSTP packet. SSTP packet type
 * (control|data) should be specified throught `type` argument.
 *
 * @param type : set packet type (Control or Data)
 * @param data : buffer to be sent
 * @param data_length : `data` length
 */
void send_sstp_packet(uint8_t type, void* data, size_t data_length)
{
  sstp_header_t sstp_header;
  size_t total_length;
  void *packet = NULL;

  total_length = sizeof(sstp_header_t) + data_length;
  memset(&sstp_header, 0, sizeof(sstp_header_t));
  
  sstp_header.version = SSTP_VERSION;
  sstp_header.reserved = type;
  sstp_header.length = htons(total_length); 
 
  packet = xmalloc(total_length);
  
  memcpy(packet, &sstp_header, sizeof(sstp_header_t));
  memcpy(packet + sizeof(sstp_header_t), data, data_length);
  
  sstp_send(packet, total_length);
  
  free(packet);
}


/**
 * Generic function to send an SSTP Data packet. Data to send is encapsulated
 * inside an SSTP packet, and transmitted througth TLS session.
 * As this function intercepts PPP packets, it is also used to detect PPP
 * negociation success, and stores NT Response code inside client chap_ctx.
 *
 * @param data : buffer to be sent
 * @param len : `data` length
 */
void send_sstp_data_packet(void* data, size_t len) 
{
  if ( ntohs(*((uint16_t*)data)) == 0xc223 )
    {
      uint8_t chap_handshake_code = *(uint8_t*)(data + 2);
      
      /* if msg is PPP-CHAP response */	
      if (chap_handshake_code == 0x02 )
	{
	  memcpy(chap_ctx, data+7, 49);
	}
    }

  send_sstp_packet(SSTP_DATA_PACKET, data, len);
}


/**
 * Generic function to send an SSTP control packet. As a control packet embeds one
 * or many attributes, they also should be specified.
 *
 * @param msg_type : SSTP control message type
 * @param attributes : pointer to the attributes buffer
 * @param attribute_number : number of attributes inside buffer
 * @param attributes_len : `attributes` length
 */
void send_sstp_control_packet(uint16_t msg_type, void* attributes,
			      uint16_t attribute_number, size_t attributes_len)
{
  sstp_control_header_t control_header;
  size_t control_length;
  uint16_t i;
  void *data, *data_ptr, *attr_ptr;

  if (attributes == NULL && attribute_number != 0)
    {
      xlog(LOG_ERROR, "No attribute specified. Cannot send message.\n");
      return;
    } 
  
  control_length = sizeof(sstp_control_header_t) + attributes_len; 
  memset(&control_header, 0, sizeof(sstp_control_header_t));
  
  /* setting control header */
  control_header.message_type = htons(msg_type);
  control_header.num_attributes = htons(attribute_number);

  if (cfg->verbose)
    {
      xlog(LOG_INFO, "\t-> Control packet\n");
      xlog(LOG_INFO, "\t-> type: %s (%x)\n",
	   control_messages_types_str[msg_type], msg_type);
      xlog(LOG_INFO, "\t-> attribute number: %d\n", attribute_number);
      xlog(LOG_INFO, "\t-> length: %d\n", control_length);
    }


  /* filling control with attributes */
  data = xmalloc(control_length);
  memcpy(data, &control_header, sizeof(sstp_control_header_t));

  attr_ptr = attributes;
  data_ptr = data + sizeof(sstp_control_header_t);
  
  for (i=0; i<attribute_number; i++)
    {
      sstp_attribute_header_t* cur_attr = (sstp_attribute_header_t*)attr_ptr;
      uint16_t plen = ntohs(cur_attr->packet_length);

      if (cfg->verbose)
	{
	  xlog(LOG_INFO, "\t\t--> Attribute %d\n", i);
	  xlog(LOG_INFO, "\t\t--> type: %s (%x)\n",
	       attr_types_str[cur_attr->attribute_id], cur_attr->attribute_id);
	  xlog(LOG_INFO, "\t\t--> length: %d\n", cur_attr->packet_length);
	}
      
      memcpy(data_ptr, attr_ptr, plen);
      attr_ptr += plen;
      data_ptr += plen;
    }

  
  /* yield to lower */
  send_sstp_packet(SSTP_CONTROL_PACKET, data, control_length);

  free(data);
}


/**
 * Allocates and fills an attribute with specified data.
 *
 * @param attribute_id : attribute code
 * @param data : data to be inserted
 * @param data_length : `data` length
 * @return a pointer to the new attribute buffer
 */
void* create_attribute(uint8_t attribute_id, void* data, size_t data_length)
{
  sstp_attribute_header_t attribute_header;
  size_t attribute_length;
  void* attribute;
  
  if (!data) return NULL;

  attribute_length = sizeof(sstp_attribute_header_t) + data_length;
  attribute = xmalloc(attribute_length);
  
  attribute_header.reserved = 0;
  attribute_header.attribute_id = attribute_id;
  attribute_header.packet_length = htons(attribute_length);
  
  memcpy(attribute, &attribute_header, sizeof(sstp_attribute_header_t));
  memcpy(attribute + sizeof(sstp_attribute_header_t), data, data_length);
  
  return attribute;
}


/**
 * On receiving a binding request, this function is triggered to prepare
 * cryptographic properties. It selects strongest negociated hash algorithm,
 * stores Nonce into client SSTP context, invoke certhash(), and changes client
 * state.
 *
 * @param data : pointer to crypto binding request packet
 * @return 0 if all good, negative value otherwise
 */
int crypto_set_binding(void* data)
{
  sstp_attribute_crypto_bind_req_t* req;
 
  /* Validating client state */
  if (ctx->state != CLIENT_CONNECT_REQUEST_SENT)
    {
      xlog(LOG_ERROR, "Incorrect message for this state\n");
      if (cfg->verbose) 
	{
	  xlog(LOG_ERROR, "Current state: %#x. Expected %#x\n",
	       ctx->state, CLIENT_CONNECT_REQUEST_SENT);
	}
      
      return -1;
    }

  /* Disable negociation timer */
  alarm(0);
  
  /* Setting crypto properties */
  req = (sstp_attribute_crypto_bind_req_t*) data;

  /* Setting hash algorithm type (SHA1 or SHA256) */
  switch(req->hash_bitmask) 
    {
    case CERT_HASH_PROTOCOL_SHA256:
      ctx->hash_algorithm = CERT_HASH_PROTOCOL_SHA256;
      break;

    case CERT_HASH_PROTOCOL_SHA1:
      ctx->hash_algorithm = CERT_HASH_PROTOCOL_SHA1;
      break;
           
    default:
      xlog(LOG_ERROR, "Unknown hash algorithm %#x\n", req->hash_bitmask);
      return -1;
    }

  memcpy(ctx->nonce, req->nonce, sizeof(uint32_t) * 8);
  
  /* compute ca file hash with chosen algorithm */
  if (crypto_set_certhash() < 0)
    return -1;
  

  /* change client state */
  set_client_status(CLIENT_CONNECT_ACK_RECEIVED);

  return 0;
}


/**
 * This function is called by crypto_set_binding() and computes server certificate
 * hash. Since certificate is provided as PEM format, it is first exported to DER
 * binary format, hashed with context-defined algorithm, and stored inside client
 * SSTP context.
 *
 * @return 0 if all good, negative value otherwise
 */
int crypto_set_certhash()
{
  int val,i;
  unsigned char dst[32];
  size_t buffer_len = 4096; 
  char buffer[buffer_len];
  unsigned char* (*HASH)();
  
  memset(buffer, 0, buffer_len);
  memset(dst, 0, 32);
  
  /* export certificate to DER format */
  val = gnutls_x509_crt_export (certificate, GNUTLS_X509_FMT_DER, buffer, &buffer_len);
  if (val != GNUTLS_E_SUCCESS) 
    {
      xlog(LOG_ERROR, "crypto_set_certhash: fail to export certificate\n");
      if (val == GNUTLS_E_SHORT_MEMORY_BUFFER)
	xlog(LOG_ERROR, "Missing memory (expected %d)\n", buffer_len);
      return -1;
    }

  /* select hash algorithm */
  if (ctx->hash_algorithm == CERT_HASH_PROTOCOL_SHA256)
    {
      val = SHA256_HASH_LEN;
      HASH = &SHA256;
    }
  else 
    {
      val = SHA1_HASH_LEN;
      HASH = &SHA1;
    }

  /* compute hash  */
  HASH(buffer, buffer_len, dst);
  
  /* move hash to client context variable */
  for(i=0; i<8; i++) ctx->certhash[i] = *(uint32_t*)(dst+(i*4));
 
  return 0;
}


/**
 * Defines Compound Mac value and store its value into SSTP client context.
 * 
 * @return 0 on SUCCESS, < 0 on ERROR
 */
int crypto_set_cmac()
{ 
  unsigned int i = 0;
  uint16_t hash_len;
  unsigned char Call_Connected_buffer[112];
  
  uint8_t hlak[32];
  uint8_t *cmac, *cmk;
  uint8_t msg[32];
  uint8_t PasswordHash[MD4_DIGEST_LENGTH];
  uint8_t PasswordHashHash[MD4_DIGEST_LENGTH];
  uint8_t NT_Response[24];
  uint8_t Master_Key[16];
  uint8_t Master_Send_Key[16];
  uint8_t Master_Receive_Key[16];

  uint8_t* ptr = NULL;

  unsigned char Call_Connected_header[16] = 
    {
      0x10, 0x01, 0x00, 0x70, 0x00, 0x04, 0x00, 0x01,
      0x00, 0x03, 0x00, 0x68, 0x00, 0x00, 0x00, 0x02
    };
  
  memset(hlak, 0, 32);
  memset(PasswordHash, 0, MD4_DIGEST_LENGTH);
  memset(PasswordHashHash, 0, MD4_DIGEST_LENGTH);
  memset(NT_Response, 0, 24);
  memset(Call_Connected_buffer, 0, 112);

  
  /* Crypto super fun time */
  
  /* Setting HLAK */
  NtPasswordHash( PasswordHash, (const uint8_t *)cfg->password, strlen(cfg->password) );
  HashNtPasswordHash( PasswordHashHash, PasswordHash );
  memcpy( NT_Response, chap_ctx->response_nt_response, 24 );
  GetMasterKey( Master_Key, PasswordHashHash, NT_Response );
  GetAsymmetricStartKey( Master_Send_Key, Master_Key, 16, TRUE, TRUE );
  GetAsymmetricStartKey( Master_Receive_Key, Master_Key,  16, FALSE, TRUE );
  
  /*
   * Specification bug: 
   * "For MS-CHAPv2, SSTP Client HLAK = MasterSendKey | MasterReceiveKey"
   * whereas implementation SHOULD be done like
   * For MS-CHAPv2, SSTP Client HLAK = MasterReceiveKey | MasterSendKey
   */
  memcpy(hlak, Master_Receive_Key, 16*sizeof(uint8_t));
  memcpy(hlak + 16, Master_Send_Key, 16*sizeof(uint8_t));

  /*
   * Computing CMAC:
   * CMac computation occurs in 2 times:
   * T1 - CMK computation : CMK = HMAC-SHA(key= hlak, msg= SEED|LEN|0x01)
   * T2 - CMac computation: CMac= HMAC-SHA(key= CMK, msg= SSTP_CALL_CONNECTED_MSG_ZEROED)
   *
   * Where SSTP_CALL_CONNECTED_MSG_ZEROED is SSTP_CALL_CONNECTED_MSG with CMAC field
   * filled with 0 (zero)
   */
  ptr = msg;
  memcpy(ptr, SSTP_SEED_PREFIX, strlen(SSTP_SEED_PREFIX));
  ptr += strlen(SSTP_SEED_PREFIX);
  
  /* T1 */
  /*
   * To generate the Compound MAC Key (CMK), implementations MUST use the HLAK, MUST use the
   * PRF+ seed value as the input to a PRF+ operation, and MUST generate 32 bytes.
   */
  
  hash_len = (ctx->hash_algorithm==CERT_HASH_PROTOCOL_SHA1) ? SHA1_HASH_LEN : SHA256_HASH_LEN;
  memcpy(ptr, &hash_len, sizeof(uint16_t)); ptr += sizeof(uint16_t);
  *ptr = 0x01; ptr ++;

  if ( (cmk = sstp_hmac(hlak, msg, 32)) == NULL)
    return -1;
  
  
  /* T2 */
  /*
   * "[...] the Compound MAC MUST be constructed from the entire 112 bytes of the Call Connected
   * message(section 2.2.11) with the Compound MAC field and Padding field zeroed out."
   */
  
  ptr = Call_Connected_buffer;
  memcpy(ptr, Call_Connected_header, 16); ptr+= 16;
  memcpy(ptr, ctx->nonce, 32); ptr += 32;
  memcpy(ptr, ctx->certhash, 32); ptr += 32;

  if ( !(cmac = sstp_hmac(cmk, Call_Connected_buffer, 112)) )
    return -1;
  
  memcpy(ctx->cmk, cmk, 32);
  memcpy(ctx->cmac, cmac, 32);

  free(cmk);
  free(cmac);

  
  /* Verbose output displays brief crypto information */
  if (cfg->verbose)
    {
      xlog(LOG_INFO, "\t\t--> hash algo\t%s (%#.2x)\n",
	   crypto_req_attrs_str[ctx->hash_algorithm], ctx->hash_algorithm);
      
      xlog(LOG_INFO, "\t\t--> nonce\t0x");
      for (i=0; i<8; i++) xlog(LOG_INFO, "%.8x", ntohl(ctx->nonce[i]));
      xlog(LOG_INFO, "\n");

      xlog(LOG_INFO, "\t\t--> CA hash\t0x");
      for(i=0; i<8; i++) xlog(LOG_INFO, "%.8x", ntohl(ctx->certhash[i]));
      xlog(LOG_INFO, "\n");

      /* debug output is much more verbose */
      if (cfg->verbose > 2)
	{
	  xlog(LOG_INFO, "\t\t--> T1 msg\t0x");
	  for (i=0; i<32; i++) xlog(LOG_INFO, "%.2x", msg[i]);
	  xlog(LOG_INFO, "\n");
	  
	  xlog(LOG_INFO, "\t\t--> H(Pwd)\t0x");
	  for (i=0; i<MD4_DIGEST_LENGTH; i++) xlog(LOG_INFO, "%.2x", PasswordHash[i]);
	  xlog(LOG_INFO, "\n");
	  
	  xlog(LOG_INFO, "\t\t--> H(H)\t0x");
	  for (i=0; i<MD4_DIGEST_LENGTH; i++) xlog(LOG_INFO, "%.2x", PasswordHashHash[i]);
	  xlog(LOG_INFO, "\n");
	  
	  xlog(LOG_INFO, "\t\t--> NT_Rsp\t0x");
	  for (i=0; i<24; i++) xlog(LOG_INFO, "%.2x", NT_Response[i]);
	  xlog(LOG_INFO, "\n");

	  xlog(LOG_INFO, "\t\t--> MKey\t0x");
	  for (i=0; i<16; i++) xlog(LOG_INFO, "%.2x", Master_Key[i]);
	  xlog(LOG_INFO, "\n");
	  
	  xlog(LOG_INFO, "\t\t--> S_Key\t0x");
	  for (i=0; i<16; i++) xlog(LOG_INFO, "%.2x", Master_Send_Key[i]);
	  xlog(LOG_INFO, "\n");
	  
	  xlog(LOG_INFO, "\t\t--> R_Key\t0x");
	  for (i=0; i<16; i++) xlog(LOG_INFO, "%.2x", Master_Receive_Key[i]);
	  xlog(LOG_INFO, "\n");
	  
	  xlog(LOG_INFO, "\t\t--> HLAK\t0x");
	  for (i=0; i<32; i++) xlog(LOG_INFO, "%.2x", hlak[i]);
	  xlog(LOG_INFO, "\n");
	  
	  xlog(LOG_INFO, "\t\t--> CMKey\t0x");
	  for(i=0;i<8;i++) xlog(LOG_INFO, "%.8x", ntohl(ctx->cmk[i]));
	  xlog(LOG_INFO, "\n");
	  
	}
            
      xlog(LOG_INFO, "\t\t--> CMac\t0x");
      for(i=0; i<8; i++)
	xlog(LOG_INFO, "%.8x", ntohl(ctx->cmac[i]));
      xlog(LOG_INFO, "\n");
      
    }

  /* disable negociation timer */
  alarm(0);
  
  return 0;
}


/**
 * Print attribute information.
 * 
 * @param data
 * @param attr_len
 */
int attribute_status_info(void* data, uint16_t attr_len)
{
  sstp_attribute_status_info_t* info;
  uint8_t attribute_id;
  uint32_t status;
  int rbytes;

  info = (sstp_attribute_status_info_t*) data;
  attribute_id = info->attrib_id;
  status = ntohl(info->status);

  /* check attribute */
  if (attribute_id > ATTRIB_STATUS_STATUS_INFO_NOT_SUPPORTED_IN_MSG)
    {
      xlog(LOG_ERROR, "Attribute id is not valid.\n");
      return -1;
    }

  if (cfg->verbose)
    {  
      /* show attribute */
      xlog(LOG_INFO, "\t\t--> attr_ref\t%s (%#.2x)\n", attr_types_str[attribute_id], attribute_id);
      xlog(LOG_INFO, "\t\t--> status\t%s (%#.2x)\n", attrib_status_str[status], status);
    }
  
  if (ctx->state != CLIENT_CONNECT_REQUEST_SENT)
    return 0;
  
  /* attrib_value is at most 64 bytes (ie full attr len <= 64 + 12 bytes)*/
  rbytes = sizeof(sstp_attribute_header_t) + 2*sizeof(uint32_t);

  while (rbytes < (64+12) && rbytes < attr_len)
    {
      uint32_t attrib_value;
      attrib_value = ntohl(*((uint32_t*)data + rbytes));
      xlog(LOG_INFO, "\t\t--> attribute value: %#.4x\n", attrib_value);
      rbytes += sizeof(uint32_t);
    }
  
  return 0;
}


/**
 * Adapted from ssl_ppp_fork() in /ssltunnel-1.18/client/main.c
 * At http://www.hsc.fr/ressources/outils/ssltunnel/download/ssltunnel-1.18.tar.gz
 * Alain Thivillon & Herve Schauer Consultants (c)
 *
 * Fork and execute pppd daemon
 * @return child pid if process is the father or error otherwise (execv pppd)
 **/
int sstp_fork() 
{
  pid_t ppp_pid;
  int retcode, amaster, aslave, i;
  struct termios pty;
  char *pppd_path;
  char *pppd_args[32];

  pppd_path = cfg->pppd_path;
  i = 0;

#if defined ___Linux___ || defined ___Darwin___
  pppd_args[i++] = "pppd"; 
  pppd_args[i++] = "nodetach";
  pppd_args[i++] = "local";
  pppd_args[i++] = "noauth";
  pppd_args[i++] = "noccp";
  pppd_args[i++] = "nobsdcomp";
  pppd_args[i++] = "user"; 
  pppd_args[i++] = cfg->username;
  pppd_args[i++] = "password";
  pppd_args[i++] = cfg->password;
  pppd_args[i++] = "sync";         /* <-- Thanks to Nicolas Collignon */
  pppd_args[i++] = "refuse-eap";
  
#elif defined ___FreeBSD___ || defined ___OpenBSD___
  pppd_args[i++] = "ppp"; 
  pppd_args[i++] = "-foreground";
  pppd_args[i++] = "-quiet";
  pppd_args[i++] = "tweety";
  
#endif

  
  if (cfg->logfile != NULL) 
    {
      pppd_args[i++] = "logfile";
      pppd_args[i++] = cfg->logfile;
      pppd_args[i++] = "debug";
    }

  if (cfg->domain != NULL)
    {
      pppd_args[i++] = "domain";
      pppd_args[i++] = cfg->domain;
    }

  pppd_args[i++] = NULL;

  memset(&pty, 0, sizeof(struct termios));
  pty.c_cc[VMIN] = 1;
  pty.c_cc[VTIME] = 0;
  pty.c_cflag |= B9600;

  retcode = openpty(&amaster, &aslave, NULL, &pty, NULL);
  if (retcode < 0)
    {
      xlog (LOG_ERROR, "openpty failed: %s", strerror(errno));
      return -1;
    }

  ppp_pid = fork();

      
  if (ppp_pid > 0)
    {
     
      dup2(amaster, 0);
      dup2(amaster, 1);
      
      if (aslave > 2) close(aslave);
      if (amaster > 2) close(amaster);     
      
      return ppp_pid;
    }
  
  else if (ppp_pid == 0) 
    {
      close(sockfd);
         
      dup2(aslave, 0);
      dup2(aslave, 1);
      
      if (aslave > 2) close (aslave);
      if (amaster > 2) close (amaster);

      
      if (execv (pppd_path, pppd_args) == -1)
	{
	  xlog (LOG_ERROR, "execv failed: %s\n", strerror(errno));
	  return -1;
	}
    }

  else 
    {
      xlog (LOG_ERROR, "sstp_fork: %s\n", strerror(errno));
      return -1;
    }

  return 0;
}


/**
 * HMAC function wrapper, this function calculates HMAC value of a n-length message
 * with the key `key`. Created HMAC buffered has to be freed later.
 * 
 * @param key is the HMAC key
 * @param d is the message to be hashed
 * @param n is `d` string length
 * @return a pointer to HMAC result buffer.
 */
uint8_t* sstp_hmac(unsigned char* key, unsigned char* d, uint16_t n)
{
  uint8_t *md = NULL;
  unsigned int mdlen;
  const EVP_MD* (*hmac)();
  unsigned int hash_len;
  
  switch (ctx->hash_algorithm) 
    {
    case CERT_HASH_PROTOCOL_SHA1:
      hmac = &EVP_sha1;
      hash_len = SHA1_HASH_LEN;
      break;
      
    case CERT_HASH_PROTOCOL_SHA256:
    default:
      hmac = &EVP_sha256;
      hash_len = SHA256_HASH_LEN;      
      break;
    }
  
  md = (uint8_t*) xmalloc(32);

  if (HMAC(hmac(), key, 32, d, n, md, &mdlen) == NULL) 
    {
      xlog(LOG_ERROR, "Failed to compute HMAC\n");
      free(md);
      return NULL;
    }
  
  if (mdlen != hash_len)
    {
      xlog(LOG_ERROR, "%s function didn't return valid data!\n",
	   crypto_req_attrs_str[ctx->hash_algorithm]);
      free(md);
      return NULL;
    }
  
  return md; 
}


/**
 * Functions defined below are only used to generate correct CMac value
 * Also see:
 * - http:tools.ietf.org/search/rfc3079
 * - http:tools.ietf.org/search/rfc2759
 */
void NtPasswordHash(uint8_t *password_hash, const uint8_t *password, size_t password_len)
{
  uint8_t buf[512];
  size_t i;

  MD4_CTX c;
  
  if (password_len > 256)
    {
      password_hash = NULL;
      return;
    }   

  memset(buf, 0, 512);
  /* Convert password into unicode */
  for (i=0; i<password_len; i++)
    {
      buf[i*2] = password[i];
    }
    
  MD4_Init(&c);
  MD4_Update(&c, buf, password_len * 2);
  MD4_Final(password_hash, &c);
}


void HashNtPasswordHash(uint8_t *password_hash_hash, const uint8_t *password_hash)
{
  MD4_CTX c;
  MD4_Init(&c);
  MD4_Update(&c, password_hash, MD4_DIGEST_LENGTH);
  MD4_Final(password_hash_hash, &c);
}


void GetMasterKey(void* MasterKey, void* PasswordHashHash, void* NTResponse)
{
  SHA_CTX c;
  uint8_t Digest[20];
  
  /*
   * "Magic" constants used in key derivations
   */
  
  static unsigned char Magic1[27] =
    {
      0x54, 0x68, 0x69, 0x73, 0x20, 0x69, 0x73, 0x20, 0x74,
      0x68, 0x65, 0x20, 0x4d, 0x50, 0x50, 0x45, 0x20, 0x4d,
      0x61, 0x73, 0x74, 0x65, 0x72, 0x20, 0x4b, 0x65, 0x79
    };

  memset(Digest, 0, sizeof(Digest));

  SHA1_Init(&c);
  SHA1_Update(&c, PasswordHashHash, 16);
  SHA1_Update(&c, NTResponse, 24);
  SHA1_Update(&c, Magic1, 27);
  SHA1_Final(Digest, &c);
  
  memcpy(MasterKey, Digest, 16);
}


void GetAsymmetricStartKey(void* MasterSessionKey, void* MasterKey, 
			   uint8_t KeyLength, uint8_t IsSend, uint8_t IsServer)
{
  uint8_t Digest[20];
  uint8_t *Magic;
  SHA_CTX c;

  /*
   * Pads used in key derivation
   */
  static unsigned char SHSpad1[40] =
    {
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };

  static unsigned char SHSpad2[40] =
    {
      0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2,
      0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2,
      0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2,
      0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2, 0xf2
    };

  /*
   * "Magic" constants used in key derivations
   */
 
  static unsigned char Magic2[84] =
    {
      0x4f, 0x6e, 0x20, 0x74, 0x68, 0x65, 0x20, 0x63, 0x6c, 0x69,
      0x65, 0x6e, 0x74, 0x20, 0x73, 0x69, 0x64, 0x65, 0x2c, 0x20,
      0x74, 0x68, 0x69, 0x73, 0x20, 0x69, 0x73, 0x20, 0x74, 0x68,
      0x65, 0x20, 0x73, 0x65, 0x6e, 0x64, 0x20, 0x6b, 0x65, 0x79,
      0x3b, 0x20, 0x6f, 0x6e, 0x20, 0x74, 0x68, 0x65, 0x20, 0x73,
      0x65, 0x72, 0x76, 0x65, 0x72, 0x20, 0x73, 0x69, 0x64, 0x65,
      0x2c, 0x20, 0x69, 0x74, 0x20, 0x69, 0x73, 0x20, 0x74, 0x68,
      0x65, 0x20, 0x72, 0x65, 0x63, 0x65, 0x69, 0x76, 0x65, 0x20,
      0x6b, 0x65, 0x79, 0x2e
    };

  static unsigned char Magic3[84] =
    {
      0x4f, 0x6e, 0x20, 0x74, 0x68, 0x65, 0x20, 0x63, 0x6c, 0x69,
      0x65, 0x6e, 0x74, 0x20, 0x73, 0x69, 0x64, 0x65, 0x2c, 0x20,
      0x74, 0x68, 0x69, 0x73, 0x20, 0x69, 0x73, 0x20, 0x74, 0x68,
      0x65, 0x20, 0x72, 0x65, 0x63, 0x65, 0x69, 0x76, 0x65, 0x20,
      0x6b, 0x65, 0x79, 0x3b, 0x20, 0x6f, 0x6e, 0x20, 0x74, 0x68,
      0x65, 0x20, 0x73, 0x65, 0x72, 0x76, 0x65, 0x72, 0x20, 0x73,
      0x69, 0x64, 0x65, 0x2c, 0x20, 0x69, 0x74, 0x20, 0x69, 0x73,
      0x20, 0x74, 0x68, 0x65, 0x20, 0x73, 0x65, 0x6e, 0x64, 0x20,
      0x6b, 0x65, 0x79, 0x2e
    };
  
  memset(Digest, 0, 20);
  
  if (IsSend)
    Magic = (IsServer ? Magic3 : Magic2);     
  else
    Magic = (IsServer ? Magic2 : Magic3);


  SHA1_Init(&c);
  SHA1_Update(&c, MasterKey, 16);
  SHA1_Update(&c, SHSpad1, 40);
  SHA1_Update(&c, Magic, 84);
  SHA1_Update(&c, SHSpad2, 40);
  SHA1_Final(Digest, &c);
  
  memcpy(MasterSessionKey, Digest, KeyLength);
}

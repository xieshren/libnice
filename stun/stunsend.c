/*
 * This file is part of the Nice GLib ICE library.
 *
 * (C) 2007 Nokia Corporation. All rights reserved.
 *  Contact: Rémi Denis-Courmont
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is the Nice GLib ICE library.
 *
 * The Initial Developers of the Original Code are Collabora Ltd and Nokia
 * Corporation. All Rights Reserved.
 *
 * Contributors:
 *   Rémi Denis-Courmont, Nokia
 *
 * Alternatively, the contents of this file may be used under the terms of the
 * the GNU Lesser General Public License Version 2.1 (the "LGPL"), in which
 * case the provisions of LGPL are applicable instead of those above. If you
 * wish to allow use of your version of this file only under the terms of the
 * LGPL and not to allow others to use your version of this file under the
 * MPL, indicate your decision by deleting the provisions above and replace
 * them with the notice and other provisions required by the LGPL. If you do
 * not delete the provisions above, a recipient may use your version of this
 * file under either the MPL or the LGPL.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <sys/types.h>
#include <sys/socket.h>

#include "stun-msg.h"

#include <assert.h>
#include <string.h>
#include <stdlib.h>

#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>

static inline
void *stun_setw (uint8_t *ptr, uint16_t value)
{
	*ptr++ = value >> 8;
	*ptr++ = value & 0xff;
	return ptr;
}


static inline
void stun_set_type (uint8_t *h, stun_class_t c, stun_method_t m)
{
	assert (c < 4);
	assert (m < (1 << 12));

	h[0] = (c >> 1) | ((m >> 6) & 0x3e);
	h[1] = ((c << 4) & 0x10) | ((m << 1) & 0xe0) | (m & 0x0f);

	assert (stun_getw (h) < (1 << 14));
	assert (stun_get_class (h) == c);
	assert (stun_get_method (h) == m);
}


static void stun_make_transid (stun_transid_t id)
{
	static struct
	{
		pthread_mutex_t lock;
		uint64_t counter;
	} store = { PTHREAD_MUTEX_INITIALIZER, 0 };
	uint64_t counter;

	pthread_mutex_lock (&store.lock);
	counter = store.counter++;
	pthread_mutex_unlock (&store.lock);

	/* FIXME: generate a random key and use HMAC or something... */
	memset (id, 0, 4);
	memcpy (id + 4, &counter, 8);
}


/**
 * Initializes a STUN message buffer, with no attributes.
 * @param c STUN message class (host byte order)
 * @param m STUN message method (host byte order)
 * @param id 12-bytes transaction ID
 */
static void stun_init (uint8_t *msg, stun_class_t c, stun_method_t m,
                       const stun_transid_t id)
{
	static const uint8_t init[8] = { 0, 0, 0, 0, 0x21, 0x12, 0xA4, 0x42 };
	memcpy (msg, init, sizeof (init));
	stun_set_type (msg, c, m);

	msg += 8;
	if (msg != id)
		memcpy (msg, id, 12);
}


/**
 * Initializes a STUN request message buffer, with no attributes.
 * @param m STUN message method (host byte order)
 */
void stun_init_request (uint8_t *req, stun_method_t m)
{
	stun_transid_t id;

	stun_make_transid (id);
	stun_init (req, STUN_REQUEST, m, id);
}


/**
 * Initializes a STUN message buffer with no attributes,
 * in response to a given valid STUN request messsage.
 * STUN method and transaction ID are copied from the request message.
 *
 * @param ans [OUT] STUN message buffer
 * @param req STUN message query
 *
 * ans == req is allowed.
 */
void stun_init_response (uint8_t *ans, const uint8_t *req)
{
	//assert (stun_valid (req));
	assert (stun_get_class (req) == STUN_REQUEST);

	stun_init (ans, STUN_RESPONSE, stun_get_method (req), stun_id (req));
}


/**
 * Reserves room for appending an attribute to an unfinished STUN message.
 * @param msg STUN message buffer
 * @param msize STUN message buffer size
 * @param type message type (host byte order)
 * @param length attribute payload byte length
 * @return a pointer to an unitialized buffer of <length> bytes to
 * where the attribute payload must be written, or NULL if there is not
 * enough room in the STUN message buffer. Return value is always on a
 * 32-bits boundary.
 */
static void *
stun_append (uint8_t *msg, size_t msize, stun_attr_type_t type, size_t length)
{
	uint16_t mlen = stun_length (msg);
	assert (stun_padding (mlen) == 0);

	if (msize > STUN_MAXMSG)
		msize = STUN_MAXMSG;

	if ((((size_t)mlen) + 24u + length) > msize)
		return NULL;

	assert (length < 0xffff);

	uint8_t *a = msg + 20u + mlen;
	a = stun_setw (a, type);
	a = stun_setw (a, length);

	mlen += 4 + length;
	/* Add padding if needed */
	memset (a + length, ' ', stun_padding (length));
	mlen += stun_padding (length);

	stun_setw (msg + 2, mlen);
	return a;
}


/**
 * Appends an attribute from memory.
 * @param msg STUN message buffer
 * @param msize STUN message buffer size
 * @param type attribute type (host byte order)
 * @param data memory address to copy payload from
 * @param len attribute payload length
 * @return 0 on success, ENOBUFS on error.
 */
static int
stun_append_bytes (uint8_t *restrict msg, size_t msize, stun_attr_type_t type,
                   const void *data, size_t len)
{
	void *ptr = stun_append (msg, msize, type, len);
	if (ptr == NULL)
		return ENOBUFS;

	memcpy (ptr, data, len);
	return 0;
}


/**
 * Appends an empty ("flag") attribute to a STUN message.
 * @param msg STUN message buffer
 * @param msize STUN message buffer size
 * @param type attribute type (host byte order)
 * @return 0 on success, ENOBUFS on error.
 */
int stun_append_flag (uint8_t *msg, size_t msize, stun_attr_type_t type)
{
	return stun_append_bytes (msg, msize, type, NULL, 0);
}


/**
 * Appends an attribute consisting of a 32-bits value to a STUN message.
 * @param msg STUN message buffer
 * @param msize STUN message buffer size
 * @param type attribute type (host byte order)
 * @param value payload (host byte order)
 * @return 0 on success, ENOBUFS on error.
 */
int
stun_append32 (uint8_t *msg, size_t msize, stun_attr_type_t type,
               uint32_t value)
{
	value = htonl (value);
	return stun_append_bytes (msg, msize, type, &value, 4);
}


/**
 * Appends an attribute consisting of a 64-bits value to a STUN message.
 * @param msg STUN message buffer
 * @param msize STUN message buffer size
 * @param type attribute type (host byte order)
 * @param value payload (host byte order)
 * @return 0 on success, ENOBUFS on error.
 */
int stun_append64 (uint8_t *msg, size_t msize, stun_attr_type_t type,
                   uint64_t value)
{
	uint32_t tab[2];
	tab[0] = htonl ((uint32_t)(value >> 32));
	tab[1] = htonl ((uint32_t)value);
	return stun_append_bytes (msg, msize, type, tab, 8);
}


/**
 * Appends an attribute from a nul-terminated string.
 * @param msg STUN message buffer
 * @param msize STUN message buffer size
 * @param type attribute type (host byte order)
 * @param str nul-terminated string
 * @return 0 on success, ENOBUFS on error.
 */
int
stun_append_string (uint8_t *restrict msg, size_t msize,
                    stun_attr_type_t type, const char *str)
{
	return stun_append_bytes (msg, msize, type, str, strlen (str));
}


/**
 * @param code host-byte order error code
 * @return a static pointer to a nul-terminated error message string.
 */
static const char *stun_strerror (stun_error_t code)
{
	static const struct
	{
		stun_error_t code;
		char     phrase[32];
	} tab[] =
	{
		{ STUN_TRY_ALTERNATE, "Try alternate server" },
		{ STUN_BAD_REQUEST, "Bad request" },
		{ STUN_UNAUTHORIZED, "Authorization required" },
		{ STUN_UNKNOWN_ATTRIBUTE, "Unknown attribute" },
		{ STUN_STALE_CREDENTIALS, "Authentication expired" },
		{ STUN_INTEGRITY_CHECK_FAILURE, "Incorrect username/password" },
		{ STUN_MISSING_USERNAME, "Username required" },
		{ STUN_USE_TLS, "Secure connection required" },
		{ STUN_MISSING_REALM, "Authentication domain required" },
		{ STUN_MISSING_NONCE, "Authentication token missing" },
		{ STUN_UNKNOWN_USERNAME, "Unknown user name" },
		{ STUN_STALE_NONCE, "Authentication token expired" },
		{ STUN_ROLE_CONFLICT, "Role conflict" },
		{ STUN_SERVER_ERROR, "Temporary server error" },
		{ STUN_GLOBAL_FAILURE, "Unrecoverable failure" },
		{ 0, "" }
	};

	for (unsigned i = 0; tab[i].phrase[0]; i++)
	{
		if (tab[i].code == code)
			return tab[i].phrase;
	}
	return "Unknown error";
}


/**
 * Appends an ERROR-CODE attribute.
 * @param msg STUN message buffer
 * @param msize STUN message buffer size
 * @param code STUN host-byte order integer error code
 * @return 0 on success, or ENOBUFS otherwise
 */
static int
stun_append_error (uint8_t *restrict msg, size_t msize, stun_error_t code)
{
	const char *str = stun_strerror (code);
	size_t len = strlen (str);
	div_t d = div (code, 100);

	uint8_t *ptr = stun_append (msg, msize, STUN_ERROR_CODE, 4 + len);
	if (ptr == NULL)
		return ENOBUFS;

	memset (ptr, 0, 2);
	assert (d.quot <= 0x7);
	ptr[2] = d.quot;
	ptr[3] = d.rem;
	memcpy (ptr + 4, str, len);
	return 0;
}


/**
 * Initializes a STUN error response message buffer with an ERROR-CODE
 * attribute, in response to a given valid STUN request messsage.
 * STUN method and transaction ID are copied from the request message.
 *
 * @param ans [OUT] STUN message buffer
 * @param msize STUN message buffer size
 * @param req STUN message to copy method and transaction ID from
 * @param err host-byte order STUN integer error code
 *
 * @return 0 on success, ENOBUFS on error
 *
 * ans == req is allowed.
 */
int stun_init_error (uint8_t *ans, size_t msize, const uint8_t *req,
                      stun_error_t err)
{
	//assert (stun_valid (req));

	stun_init (ans, STUN_ERROR, stun_get_method (req), stun_id (req));
	return stun_append_error (ans, msize, err);
}


/**
 * Initializes a STUN error response message buffer, in response to a valid
 * STUN request messsage with unknown attributes. STUN method, transaction ID
 * and unknown attribute IDs are copied from the request message.
 *
 * @param ans [OUT] STUN message buffer
 * @param msize STUN message buffer size
 * @param req STUN request message
 * @return 0 on success, ENOBUFS otherwise
 *
 * ans == req is allowed.
 */
int stun_init_error_unknown (uint8_t *ans, size_t msize, const uint8_t *req)
{
	uint16_t ids[stun_length (req) / 4];
	unsigned counter;

	//assert (stun_valid (req));
	assert (stun_get_class (req) == STUN_REQUEST);

	counter = stun_find_unknown (req, ids, sizeof (ids) / sizeof (ids[0]));
	assert (counter > 0);

	if (stun_init_error (ans, msize, req, STUN_UNKNOWN_ATTRIBUTE))
		return ENOBUFS;

	for (unsigned i = 0; i < counter; i++)
		ids[i] = htons (ids[i]);
	return stun_append_bytes (ans, msize, STUN_UNKNOWN_ATTRIBUTES, ids,
	                          counter * 2);
}


/**
 * Appends an attribute consisting of a network address to a STUN message.
 * @param msg STUN message buffer
 * @param msize STUN message buffer size
 * @param type attribyte type (host byte order)
 * @param addr socket address to convert into an attribute
 * @param addrlen byte length of socket address
 * @return 0 on success, ENOBUFS if the message buffer overflowed,
 * EAFNOSUPPORT is the socket address family is not supported,
 * EINVAL if the socket address length is too small w.r.t. the address family.
 */
int
stun_append_addr (uint8_t *restrict msg, size_t msize, stun_attr_type_t type,
                  const struct sockaddr *restrict addr, socklen_t addrlen)
{
	if (addrlen < sizeof (struct sockaddr))
		return EINVAL;

	const void *pa;
	uint16_t alen, port;
	uint8_t family;

	switch (addr->sa_family)
	{
		case AF_INET:
		{
			const struct sockaddr_in *ip4 = (const struct sockaddr_in *)addr;
			assert (addrlen >= sizeof (*ip4));
			family = 1;
			port = ip4->sin_port;
			alen = 4;
			pa = &ip4->sin_addr;
			break;
		}

		case AF_INET6:
		{
			const struct sockaddr_in6 *ip6 = (const struct sockaddr_in6 *)addr;
			if (addrlen < sizeof (*ip6))
				return EINVAL;

			family = 2;
			port = ip6->sin6_port;
			alen = 16;
			pa = &ip6->sin6_addr;
			break;
		}

		default:
			return EAFNOSUPPORT;
	}

	uint8_t *ptr = stun_append (msg, msize, type, 4 + alen);
	if (ptr == NULL)
		return ENOBUFS;

	ptr[0] = 0;
	ptr[1] = family;
	memcpy (ptr + 2, &port, 2);
	memcpy (ptr + 4, pa, alen);
	return 0;
}


/**
 * Appends an attribute consisting of a xor'ed network address.
 * @param msg STUN message buffer
 * @param msize STUN message buffer size
 * @param type attribyte type (host byte order)
 * @param addr socket address to convert into an attribute
 * @param addrlen byte length of socket address
 * @return 0 on success, ENOBUFS if the message buffer overflowed,
 * EAFNOSUPPORT is the socket address family is not supported,
 * EINVAL if the socket address length is too small w.r.t. the address family.
 */
int stun_append_xor_addr (uint8_t *restrict msg, size_t msize,
                          stun_attr_type_t type,
                          const struct sockaddr *restrict addr,
                          socklen_t addrlen)
{
	union
	{
		struct sockaddr addr;
		char buf[addrlen];
	} xor;
	int val;

	memcpy (xor.buf, addr, addrlen);
	val = stun_xor_address (msg, &xor.addr, addrlen);
	if (val)
		return val;

	return stun_append_addr (msg, msize, type, &xor.addr, addrlen);
}


static size_t
stun_finish_long (uint8_t *msg, size_t *restrict plen,
                  const char *realm, const char *username,
                  const void *key, size_t keylen,
                  const void *nonce, size_t noncelen)
{
	assert (plen != NULL);

	size_t len = *plen;
	uint8_t *sha = NULL;
	int val = ENOBUFS;

	if (realm != NULL)
	{
		val = stun_append_string (msg, len, STUN_REALM, realm);
		if (val)
			return val;
	}

	if (username != NULL)
	{
		val = stun_append_string (msg, len, STUN_USERNAME, username);
		if (val)
			return val;
	}

	if (nonce != NULL)
	{
		val = stun_append_bytes (msg, len, STUN_NONCE, nonce, noncelen);
		if (val)
			return val;
	}

	if (key != NULL)
	{
		sha = stun_append (msg, len, STUN_MESSAGE_INTEGRITY, 20);
		if (sha == NULL)
			return ENOBUFS;
	}

	void *crc = stun_append (msg, len, STUN_FINGERPRINT, 4);
	if (crc == NULL)
		return ENOBUFS;

	if (sha != NULL)
	{
		stun_sha1 (msg, sha, key, keylen);
#if 0
		DBG (" Message HMAC-SHA1 fingerprint:"
		     "\n  key     : 0x");
		for (unsigned i = 0; i < keylen; i++)
			DBG ("%02x", ((uint8_t *)key)[i]);
		DBG ("\n  sent    : 0x");
		for (unsigned i = 0; i < 20; i++)
			DBG ("%02x", sha[i]);
		DBG ("\n");
#endif
	}

	uint32_t fpr = htonl (stun_fingerprint (msg));
	memcpy (crc, &fpr, sizeof (fpr));

	*plen = 20u + stun_length (msg);
	return 0;
}


size_t stun_finish_short (uint8_t *msg, size_t *restrict plen,
                          const char *username, const char *password,
                          const void *nonce, size_t noncelen)
{
	size_t passlen = (password != NULL) ? strlen (password) : 0;
	return stun_finish_long (msg, plen, NULL, username, password, passlen,
	                         nonce, noncelen);
}


size_t stun_finish (uint8_t *msg, size_t *restrict plen)
{
	return stun_finish_short (msg, plen, NULL, NULL, NULL, 0);
}


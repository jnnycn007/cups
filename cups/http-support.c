//
// HTTP support routines for CUPS.
//
// Copyright © 2020-2025 by OpenPrinting
// Copyright © 2007-2019 by Apple Inc.
// Copyright © 1997-2007 by Easy Software Products, all rights reserved.
//
// Licensed under Apache License v2.0.  See the file "LICENSE" for more
// information.
//

#include "cups-private.h"
#include "dnssd.h"


//
// Local types...
//

typedef struct _http_uribuf_s		// URI buffer
{
  cups_dnssd_t		*dnssd;		// DNS-SD context
  char			*buffer;	// Pointer to buffer
  size_t		bufsize;	// Size of buffer
  http_resolve_t	options;	// Options passed to httpResolveURI
  const char		*resource;	// Resource from URI
  const char		*uuid;		// UUID from URI
} _http_uribuf_t;


//
// Local globals...
//

static const char * const http_days[7] =// Days of the week
			{
			  "Sun",
			  "Mon",
			  "Tue",
			  "Wed",
			  "Thu",
			  "Fri",
			  "Sat"
			};
static const char * const http_months[12] =
			{		// Months of the year
			  "Jan",
			  "Feb",
			  "Mar",
			  "Apr",
			  "May",
			  "Jun",
		          "Jul",
			  "Aug",
			  "Sep",
			  "Oct",
			  "Nov",
			  "Dec"
			};
static const char * const http_states[] =
			{		// HTTP state strings
			  "ERROR",
			  "WAITING",
			  "OPTIONS",
			  "GET",
			  "GET-send",
			  "HEAD",
			  "POST",
			  "POST-recv",
			  "POST-send",
			  "PUT",
			  "PUT-recv",
			  "DELETE",
			  "TRACE",
			  "CONNECT",
			  "STATUS",
			  "UNKNOWN_METHOD",
			  "UNKNOWN_VERSION"
			};


//
// Local functions...
//

static const char	*http_copy_decode(char *dst, const char *src, size_t dstsize, const char *term, int decode);
static char		*http_copy_encode(char *dst, const char *src, char *dstend, const char *reserved, const char *term, int encode);
static void 		http_resolve_cb(cups_dnssd_resolve_t *res, void *cb_data, cups_dnssd_flags_t flags, uint32_t if_index, const char *fullname, const char *host, uint16_t port, int num_txt, cups_option_t *txt);


//
// 'httpAssembleURI()' - Assemble a uniform resource identifier from its
//                       components.
//
// This function escapes reserved characters in the URI depending on the
// value of the "encoding" argument.  You should use this function in
// place of traditional string functions whenever you need to create a
// URI string.
//
// @since CUPS 1.2@
//

http_uri_status_t			// O - URI status
httpAssembleURI(
    http_uri_coding_t encoding,		// I - Encoding flags
    char              *uri,		// I - URI buffer
    int               urilen,		// I - Size of URI buffer
    const char        *scheme,		// I - Scheme name
    const char        *username,	// I - Username
    const char        *host,		// I - Hostname or address
    int               port,		// I - Port number
    const char        *resource)	// I - Resource
{
  char		*ptr,			// Pointer into URI buffer
		*end;			// End of URI buffer


  // Range check input...
  if (!uri || urilen < 1 || !scheme || port < 0)
  {
    if (uri)
      *uri = '\0';

    return (HTTP_URI_STATUS_BAD_ARGUMENTS);
  }

  // Assemble the URI starting with the scheme...
  end = uri + urilen - 1;
  ptr = http_copy_encode(uri, scheme, end, NULL, NULL, 0);

  if (!ptr)
    goto assemble_overflow;

  if (!strcmp(scheme, "geo") || !strcmp(scheme, "mailto") || !strcmp(scheme, "tel"))
  {
    // geo:, mailto:, and tel: only have :, no //...
    if (ptr < end)
      *ptr++ = ':';
    else
      goto assemble_overflow;
  }
  else
  {
    // Schemes other than geo:, mailto:, and tel: typically have //...
    if ((ptr + 2) < end)
    {
      *ptr++ = ':';
      *ptr++ = '/';
      *ptr++ = '/';
    }
    else
    {
      goto assemble_overflow;
    }
  }

  // Next the username and hostname, if any...
  if (host)
  {
    const char	*hostptr;		// Pointer into hostname
    int		have_ipv6;		// Do we have an IPv6 address?

    if (username && *username)
    {
      // Add username@ first...
      ptr = http_copy_encode(ptr, username, end, "/?#[]@", NULL, encoding & HTTP_URI_CODING_USERNAME);

      if (!ptr)
        goto assemble_overflow;

      if (ptr < end)
	*ptr++ = '@';
      else
        goto assemble_overflow;
    }

    // Then add the hostname.  Since IPv6 is a particular pain to deal
    // with, we have several special cases to deal with.  If we get
    // an IPv6 address with brackets around it, assume it is already in
    // URI format.  Since DNS-SD service names can sometimes look like
    // raw IPv6 addresses, we specifically look for "._tcp" in the name,
    // too...
    for (hostptr = host, have_ipv6 = strchr(host, ':') && !strstr(host, "._tcp"); *hostptr && have_ipv6; hostptr ++)
    {
      if (*hostptr != ':' && !isxdigit(*hostptr & 255))
      {
        have_ipv6 = *hostptr == '%';
        break;
      }
    }

    if (have_ipv6)
    {
      // We have a raw IPv6 address...
      if (strchr(host, '%') && !(encoding & HTTP_URI_CODING_RFC6874))
      {
        // We have a link-local address, add "[v1." prefix...
	if ((ptr + 4) < end)
	{
	  *ptr++ = '[';
	  *ptr++ = 'v';
	  *ptr++ = '1';
	  *ptr++ = '.';
	}
	else
	{
          goto assemble_overflow;
        }
      }
      else
      {
        // We have a normal (or RFC 6874 link-local) address, add "[" prefix...
	if (ptr < end)
	  *ptr++ = '[';
	else
          goto assemble_overflow;
      }

      // Copy the rest of the IPv6 address, and terminate with "]".
      while (ptr < end && *host)
      {
        if (*host == '%')
        {
          // Convert/encode zone separator
          if (encoding & HTTP_URI_CODING_RFC6874)
          {
            if (ptr >= (end - 2))
              goto assemble_overflow;

            *ptr++ = '%';
            *ptr++ = '2';
            *ptr++ = '5';
          }
          else
          {
	    *ptr++ = '+';
	  }

	  host ++;
	}
	else
	{
	  *ptr++ = *host++;
	}
      }

      if (*host)
        goto assemble_overflow;

      if (ptr < end)
	*ptr++ = ']';
      else
        goto assemble_overflow;
    }
    else
    {
      // Otherwise, just copy the host string (the extra chars are not in the
      // "reg-name" ABNF rule; anything <= SP or >= DEL plus % gets automatically
      // percent-encoded.
      ptr = http_copy_encode(ptr, host, end, "\"#/:<>?@[\\]^`{|}", NULL, encoding & HTTP_URI_CODING_HOSTNAME);

      if (!ptr)
        goto assemble_overflow;
    }

    // Finish things off with the port number...
    if (!strcmp(scheme, "http") && port == 80)
      port = 0;
    else if (!strcmp(scheme, "https") && port == 443)
      port = 0;
    else if ((!strcmp(scheme, "ipp") || !strcmp(scheme, "ipps")) && port == 631)
      port = 0;

    if (port > 0)
    {
      snprintf(ptr, (size_t)(end - ptr + 1), ":%d", port);
      ptr += strlen(ptr);

      if (ptr >= end)
	goto assemble_overflow;
    }
  }

  // Last but not least, add the resource string...
  if (resource)
  {
    char	*query;			// Pointer to query string

    // Copy the resource string up to the query string if present...
    query = strchr(resource, '?');
    ptr   = http_copy_encode(ptr, resource, end, NULL, "?", encoding & HTTP_URI_CODING_RESOURCE);
    if (!ptr)
      goto assemble_overflow;

    if (query)
    {
      // Copy query string without encoding...
      ptr = http_copy_encode(ptr, query, end, NULL, NULL, encoding & HTTP_URI_CODING_QUERY);
      if (!ptr)
	goto assemble_overflow;
    }
  }
  else if (ptr < end)
  {
    *ptr++ = '/';
  }
  else
  {
    goto assemble_overflow;
  }

  // Nul-terminate the URI buffer and return with no errors...
  *ptr = '\0';

  return (HTTP_URI_STATUS_OK);

  // Clear the URI string and return an overflow error; I don't usually
  // like goto's, but in this case it makes sense...
  assemble_overflow:

  *uri = '\0';
  return (HTTP_URI_STATUS_OVERFLOW);
}


//
// 'httpAssembleURIf()' - Assemble a uniform resource identifier from its components with a formatted resource.
//
// This function creates a formatted version of the resource string
// argument "resourcef" and escapes reserved characters in the URI
// depending on the value of the "encoding" argument.  You should use
// this function in place of traditional string functions whenever
// you need to create a URI string.
//
// @since CUPS 1.2@
//

http_uri_status_t			// O - URI status
httpAssembleURIf(
    http_uri_coding_t encoding,		// I - Encoding flags
    char              *uri,		// I - URI buffer
    int               urilen,		// I - Size of URI buffer
    const char        *scheme,		// I - Scheme name
    const char        *username,	// I - Username
    const char        *host,		// I - Hostname or address
    int               port,		// I - Port number
    const char        *resourcef,	// I - Printf-style resource
    ...)				// I - Additional arguments as needed
{
  va_list	ap;			// Pointer to additional arguments
  char		resource[1024];		// Formatted resource string
  int		bytes;			// Bytes in formatted string


  // Range check input...
  if (!uri || urilen < 1 || !scheme || port < 0 || !resourcef)
  {
    if (uri)
      *uri = '\0';

    return (HTTP_URI_STATUS_BAD_ARGUMENTS);
  }

  // Format the resource string and assemble the URI...
  va_start(ap, resourcef);
  bytes = vsnprintf(resource, sizeof(resource), resourcef, ap);
  va_end(ap);

  if ((size_t)bytes >= sizeof(resource))
  {
    *uri = '\0';
    return (HTTP_URI_STATUS_OVERFLOW);
  }
  else
  {
    return (httpAssembleURI(encoding,  uri, urilen, scheme, username, host, port, resource));
  }
}


//
// 'httpAssembleUUID()' - Assemble a name-based UUID URN conforming to RFC 4122.
//
// This function creates a unique 128-bit identifying number using the server
// name, port number, random data, and optionally an object name and/or object
// number.  The result is formatted as a UUID URN as defined in RFC 4122.
//
// The buffer needs to be at least 46 bytes in size.
//
// @since CUPS 1.7@
//

char *					// I - UUID string
httpAssembleUUID(const char *server,	// I - Server name
		 int        port,	// I - Port number
		 const char *name,	// I - Object name or NULL
		 int        number,	// I - Object number or 0
		 char       *buffer,	// I - String buffer
		 size_t     bufsize)	// I - Size of buffer
{
  char			data[1024];	// Source string for MD5
  unsigned char		md5sum[16];	// MD5 digest/sum


  // Build a version 3 UUID conforming to RFC 4122.
  //
  // Start with the MD5 sum of the server, port, object name and
  // number, and some random data on the end.
  snprintf(data, sizeof(data), "%s:%d:%s:%d:%04x:%04x", server, port, name ? name : server, number, (unsigned)CUPS_RAND() & 0xffff, (unsigned)CUPS_RAND() & 0xffff);

  cupsHashData("md5", (unsigned char *)data, strlen(data), md5sum, sizeof(md5sum));

  // Generate the UUID from the MD5...
  snprintf(buffer, bufsize, "urn:uuid:%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", md5sum[0], md5sum[1], md5sum[2], md5sum[3], md5sum[4], md5sum[5], (md5sum[6] & 15) | 0x30, md5sum[7], (md5sum[8] & 0x3f) | 0x40, md5sum[9], md5sum[10], md5sum[11], md5sum[12], md5sum[13], md5sum[14], md5sum[15]);

  return (buffer);
}


//
// 'httpDecode64()' - Base64-decode a string.
//
// This function is deprecated. Use the httpDecode64_2() function instead
// which provides buffer length arguments.
//
// @deprecated@ @exclude all@
//

char *					// O - Decoded string
httpDecode64(char       *out,		// I - String to write to
             const char *in)		// I - String to read from
{
  size_t	outlen;			// Output buffer length


  // Use the old maximum buffer size for binary compatibility...
  outlen = 512;

  return (httpDecode64_3(out, &outlen, in, NULL));
}


//
// 'httpDecode64_2()' - Base64-decode a string.
//
// The caller must initialize "outlen" to the maximum size of the decoded
// string before calling @code httpDecode64_2@.  On return "outlen" contains the
// decoded length of the string.
//
// @deprecsted@ @exclude all@
//

char *					// O  - Decoded string
httpDecode64_2(char       *out,		// I  - String to write to
	       int        *outlen,	// IO - Size of output string
               const char *in)		// I  - String to read from
{
  size_t	templen;		// Temporary length
  char		*ret;			// Return value


  // Range check input...
  if (!outlen || *outlen < 1)
    return (NULL);

  // Decode...
  templen = (size_t)*outlen;
  ret     = httpDecode64_3(out, &templen, in, NULL);

  // Save return values...
  *outlen = (int)templen;

  return (ret);
}


//
// 'httpDecode64_3()' - Base64-decode a string.
//
// This function decodes a Base64 string as defined by RFC 4648.  The caller
// must initialize "outlen" to the maximum size of the decoded string.  On
// return "outlen" contains the decoded length of the string and "end" (if not
// `NULL`) points to the end of the Base64 data that has been decoded.
//
// This function always reserves one byte in the output buffer for a nul
// terminating character, even if the result is not a regular string.  Callers
// should ensure that the output buffer is at least one byte larger than the
// expected size, for example 33 bytes for a SHA-256 hash which is 32 bytes in
// length.
//
// This function supports both Base64 and Base64url strings.
//
// @since CUPS 2.5@
//

char *					// O  - Decoded string or `NULL` on error
httpDecode64_3(char       *out,		// I  - String to write to
	       size_t     *outlen,	// IO - Size of output string
               const char *in,		// I  - String to read from
               const char **end)	// O  - Pointer to end of Base64 data (`NULL` if don't care)
{
  int		pos;			// Bit position
  unsigned	base64;			// Value of this character
  char		*outptr,		// Output pointer
		*outend;		// End of output buffer


  // Range check input...
  if (!out || !outlen || *outlen < 1 || !in)
    return (NULL);

  if (!*in)
  {
    *out    = '\0';
    *outlen = 0;

    if (end)
      *end = in;

    return (out);
  }

  // Convert from base-64 to bytes...
  for (outptr = out, outend = out + *outlen - 1, pos = 0; *in != '\0'; in ++)
  {
    // Decode this character into a number from 0 to 63...
    if (*in >= 'A' && *in <= 'Z')
      base64 = (unsigned)(*in - 'A');
    else if (*in >= 'a' && *in <= 'z')
      base64 = (unsigned)(*in - 'a' + 26);
    else if (*in >= '0' && *in <= '9')
      base64 = (unsigned)(*in - '0' + 52);
    else if (*in == '+' || *in == '-')
      base64 = 62;
    else if (*in == '/' || *in == '_')
      base64 = 63;
    else if (*in == '=')
      break;
    else if (isspace(*in & 255))
      continue;
    else
      break;

    // Store the result in the appropriate chars...
    switch (pos)
    {
      case 0 :
          if (outptr < outend)
            *outptr = (char)(base64 << 2);
	  pos ++;
	  break;
      case 1 :
          if (outptr < outend)
            *outptr++ |= (char)((base64 >> 4) & 3);
          if (outptr < outend)
	    *outptr = (char)((base64 << 4) & 255);
	  pos ++;
	  break;
      case 2 :
          if (outptr < outend)
            *outptr++ |= (char)((base64 >> 2) & 15);
          if (outptr < outend)
	    *outptr = (char)((base64 << 6) & 255);
	  pos ++;
	  break;
      case 3 :
          if (outptr < outend)
            *outptr++ |= (char)base64;
	  pos = 0;
	  break;
    }
  }

  // Add a trailing nul...
  *outptr = '\0';

  // Skip trailing '='...
  while (*in == '=')
    in ++;

  // Return the decoded string, next input pointer, and size...
  *outlen = (size_t)(outptr - out);

  if (end)
    *end = in;

  return (out);
}


//
// 'httpEncode64()' - Base64-encode a string.
//
// This function is deprecated. Use the httpEncode64_2() function instead
// which provides buffer length arguments.
//
// @deprecated@ @exclude all@
//

char *					// O - Encoded string
httpEncode64(char       *out,		// I - String to write to
             const char *in)		// I - String to read from
{
  return (httpEncode64_3(out, 512, in, (size_t)strlen(in), false));
}


//
// 'httpEncode64_2()' - Base64-encode a string.
//
// @deprecated@ @exclude all@
//

char *					// O - Encoded string
httpEncode64_2(char       *out,		// I - String to write to
	       int        outlen,	// I - Maximum size of output string
               const char *in,		// I - String to read from
	       int        inlen)	// I - Size of input string
{
  return (httpEncode64_3(out, (size_t)outlen, in, (size_t)inlen, false));
}


//
// 'httpEncode64_3()' - Base64-encode a string.
//
// This function encodes a Base64 string as defined by RFC 4648.  The "url"
// parameter controls whether the original Base64 ("url" = `false`) or the
// Base64url ("url" = `true`) alphabet is used.
//
// @since CUPS 2.5@
//

char *					// O - Encoded string
httpEncode64_3(char       *out,		// I - String to write to
	       size_t     outlen,	// I - Maximum size of output string
               const char *in,		// I - String to read from
	       size_t     inlen,	// I - Size of input string
	       bool       url)		// I - `true` for Base64url, `false` for Base64
{
  char		*outptr,		// Output pointer
		*outend;		// End of output buffer
  const char	*alpha;			// Alphabet
  static const char *base64 =		// Base64 alphabet
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  static const char *base64url =	// Base64url alphabet
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";


  // Range check input...
  if (!out || outlen < 1 || !in)
    return (NULL);

  // Encode bytes...
  alpha = url ? base64url : base64;

  for (outptr = out, outend = out + outlen - 1; inlen > 0; in ++, inlen --)
  {
    // Encode the up to 3 characters as 4 Base64 numbers...
    if (outptr < outend)
      *outptr ++ = alpha[(in[0] & 255) >> 2];

    if (outptr < outend)
    {
      if (inlen > 1)
        *outptr ++ = alpha[(((in[0] & 255) << 4) | ((in[1] & 255) >> 4)) & 63];
      else
        *outptr ++ = alpha[((in[0] & 255) << 4) & 63];
    }

    in ++;
    inlen --;
    if (inlen <= 0)
    {
      if (!url && outptr < outend)
	*outptr ++ = '=';
      if (!url && outptr < outend)
	*outptr ++ = '=';
      break;
    }

    if (outptr < outend)
    {
      if (inlen > 1)
        *outptr ++ = alpha[(((in[0] & 255) << 2) | ((in[1] & 255) >> 6)) & 63];
      else
        *outptr ++ = alpha[((in[0] & 255) << 2) & 63];
    }

    in ++;
    inlen --;
    if (inlen <= 0)
    {
      if (!url && outptr < outend)
        *outptr ++ = '=';
      break;
    }

    if (outptr < outend)
      *outptr ++ = alpha[in[0] & 63];
  }

  *outptr = '\0';

  // Return the encoded string...
  return (out);
}


//
// 'httpGetDateString()' - Get a formatted date/time string from a time value.
//
// @deprecated@ @exclude all@
//

const char *				// O - Date/time string
httpGetDateString(time_t t)		// I - Time in seconds
{
  _cups_globals_t *cg = _cupsGlobals();	// Pointer to library globals


  return (httpGetDateString2(t, cg->http_date, sizeof(cg->http_date)));
}


//
// 'httpGetDateString2()' - Get a formatted date/time string from a time value.
//
// @since CUPS 1.2@
//

const char *				// O - Date/time string
httpGetDateString2(time_t t,		// I - Time in seconds
                   char   *s,		// I - String buffer
		   int    slen)		// I - Size of string buffer
{
  struct tm	tdate;			// UNIX date/time data


  gmtime_r(&t, &tdate);

  snprintf(s, (size_t)slen, "%s, %02d %s %d %02d:%02d:%02d GMT", http_days[tdate.tm_wday], tdate.tm_mday, http_months[tdate.tm_mon], tdate.tm_year + 1900, tdate.tm_hour, tdate.tm_min, tdate.tm_sec);

  return (s);
}


//
// 'httpGetDateTime()' - Get a time value from a formatted date/time string.
//

time_t					// O - Time in seconds
httpGetDateTime(const char *s)		// I - Date/time string
{
  int		i;			// Looping var
  char		mon[16];		// Abbreviated month name
  int		day, year;		// Day of month and year
  int		hour, min, sec;		// Time
  int		days;			// Number of days since 1970
  static const int normal_days[] =	// Days to a month, normal years
		{ 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365 };
  static const int leap_days[] =	// Days to a month, leap years
		{ 0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366 };


  DEBUG_printf("2httpGetDateTime(s=\"%s\")", s);

  // Extract the date and time from the formatted string...
  if (sscanf(s, "%*s%d%15s%d%d:%d:%d", &day, mon, &year, &hour, &min, &sec) < 6)
    return (0);

  DEBUG_printf("4httpGetDateTime: day=%d, mon=\"%s\", year=%d, hour=%d, min=%d, sec=%d", day, mon, year, hour, min, sec);

  // Check for invalid year (RFC 7231 says it's 4DIGIT)
  if (year > 9999)
    return (0);

  // Convert the month name to a number from 0 to 11.
  for (i = 0; i < 12; i ++)
  {
    if (!_cups_strcasecmp(mon, http_months[i]))
      break;
  }

  if (i >= 12)
    return (0);

  DEBUG_printf("4httpGetDateTime: i=%d", i);

  // Now convert the date and time to a UNIX time value in seconds since
  // 1970.  We can't use mktime() since the timezone may not be UTC but
  // the date/time string *is* UTC.
  if ((year & 3) == 0 && ((year % 100) != 0 || (year % 400) == 0))
    days = leap_days[i] + day - 1;
  else
    days = normal_days[i] + day - 1;

  DEBUG_printf("4httpGetDateTime: days=%d", days);

  days += (year - 1970) * 365 +		// 365 days per year (normally)
          ((year - 1) / 4 - 492) -	// + leap days
	  ((year - 1) / 100 - 19) +	// - 100 year days
          ((year - 1) / 400 - 4);	// + 400 year days

  DEBUG_printf("4httpGetDateTime: days=%d\n", days);

  return (days * 86400 + hour * 3600 + min * 60 + sec);
}


//
// 'httpSeparate()' - Separate a Universal Resource Identifier into its
//                    components.
//
// This function is deprecated; use the httpSeparateURI() function instead.
//
// @deprecated@ @exclude all@
//

void
httpSeparate(const char *uri,		// I - Universal Resource Identifier
             char       *scheme,	// O - Scheme [32] (http, https, etc.)
	     char       *username,	// O - Username [1024]
	     char       *host,		// O - Hostname [1024]
	     int        *port,		// O - Port number to use
             char       *resource)	// O - Resource/filename [1024]
{
  httpSeparateURI(HTTP_URI_CODING_ALL, uri, scheme, 32, username, HTTP_MAX_URI, host, HTTP_MAX_URI, port, resource, HTTP_MAX_URI);
}


//
// 'httpSeparate2()' - Separate a Universal Resource Identifier into its
//                     components.
//
// This function is deprecated; use the httpSeparateURI() function instead.
//
// @deprecated@ @exclude all@
//

void
httpSeparate2(const char *uri,		// I - Universal Resource Identifier
              char       *scheme,	// O - Scheme (http, https, etc.)
	      int        schemelen,	// I - Size of scheme buffer
	      char       *username,	// O - Username
	      int        usernamelen,	// I - Size of username buffer
	      char       *host,		// O - Hostname
	      int        hostlen,	// I - Size of hostname buffer
	      int        *port,		// O - Port number to use
              char       *resource,	// O - Resource/filename
	      int        resourcelen)	// I - Size of resource buffer
{
  httpSeparateURI(HTTP_URI_CODING_ALL, uri, scheme, schemelen, username, usernamelen, host, hostlen, port, resource, resourcelen);
}


//
// 'httpSeparateURI()' - Separate a Universal Resource Identifier into its
//                       components.
//
// @since CUPS 1.2@
//

http_uri_status_t			// O - Result of separation
httpSeparateURI(
    http_uri_coding_t decoding,		// I - Decoding flags
    const char        *uri,		// I - Universal Resource Identifier
    char              *scheme,		// O - Scheme (http, https, etc.)
    int               schemelen,	// I - Size of scheme buffer
    char              *username,	// O - Username
    int               usernamelen,	// I - Size of username buffer
    char              *host,		// O - Hostname
    int               hostlen,		// I - Size of hostname buffer
    int               *port,		// O - Port number to use
    char              *resource,	// O - Resource/filename
    int               resourcelen)	// I - Size of resource buffer
{
  char			*ptr,		// Pointer into string...
			*end;		// End of string
  const char		*sep;		// Separator character
  http_uri_status_t	status;		// Result of separation


  // Initialize everything to blank...
  if (scheme && schemelen > 0)
    *scheme = '\0';

  if (username && usernamelen > 0)
    *username = '\0';

  if (host && hostlen > 0)
    *host = '\0';

  if (port)
    *port = 0;

  if (resource && resourcelen > 0)
    *resource = '\0';

  // Range check input...
  if (!uri || !port || !scheme || schemelen <= 0 || !username || usernamelen <= 0 || !host || hostlen <= 0 || !resource || resourcelen <= 0)
    return (HTTP_URI_STATUS_BAD_ARGUMENTS);

  if (!*uri)
    return (HTTP_URI_STATUS_BAD_URI);

  // Grab the scheme portion of the URI...
  status = HTTP_URI_STATUS_OK;

  if (!strncmp(uri, "//", 2))
  {
    // Workaround for HP IPP client bug...
    cupsCopyString(scheme, "ipp", (size_t)schemelen);
    status = HTTP_URI_STATUS_MISSING_SCHEME;
  }
  else if (*uri == '/')
  {
    // Filename...
    cupsCopyString(scheme, "file", (size_t)schemelen);
    status = HTTP_URI_STATUS_MISSING_SCHEME;
  }
  else
  {
    // Standard URI with scheme...
    for (ptr = scheme, end = scheme + schemelen - 1; *uri && *uri != ':' && ptr < end;)
    {
      if (strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                 "abcdefghijklmnopqrstuvwxyz"
		 "0123456789-+.", *uri) != NULL)
        *ptr++ = *uri++;
      else
        break;
    }

    *ptr = '\0';

    if (*uri != ':' || *scheme == '.' || !*scheme)
    {
      *scheme = '\0';
      return (HTTP_URI_STATUS_BAD_SCHEME);
    }

    uri ++;
  }

  // Set the default port number...
  if (!strcmp(scheme, "http"))
    *port = 80;
  else if (!strcmp(scheme, "https"))
    *port = 443;
  else if (!strcmp(scheme, "ipp") || !strcmp(scheme, "ipps"))
    *port = 631;
  else if (!_cups_strcasecmp(scheme, "lpd"))
    *port = 515;
  else if (!strcmp(scheme, "socket"))	// Not yet registered with IANA...
    *port = 9100;
  else if (strcmp(scheme, "file") && strcmp(scheme, "mailto") && strcmp(scheme, "tel"))
    status = HTTP_URI_STATUS_UNKNOWN_SCHEME;

  // Now see if we have a hostname...
  if (!strncmp(uri, "//", 2))
  {
    // Yes, extract it...
    uri += 2;

    // Grab the username, if any...
    if ((sep = strpbrk(uri, "@/")) != NULL && *sep == '@')
    {
      // Get a username:password combo...
      uri = http_copy_decode(username, uri, (size_t)usernamelen, "@", decoding & HTTP_URI_CODING_USERNAME);

      if (!uri)
      {
        *username = '\0';
        return (HTTP_URI_STATUS_BAD_USERNAME);
      }

      uri ++;
    }

    // Then the hostname/IP address...
    if (*uri == '[')
    {
      // Grab IPv6 address...
      uri ++;
      if (*uri == 'v')
      {
        // Skip IPvFuture ("vXXXX.") prefix...
        uri ++;

        while (isxdigit(*uri & 255))
          uri ++;

        if (*uri != '.')
        {
	  *host = '\0';
	  return (HTTP_URI_STATUS_BAD_HOSTNAME);
        }

        uri ++;
      }

      uri = http_copy_decode(host, uri, (size_t)hostlen, "]", decoding & HTTP_URI_CODING_HOSTNAME);

      if (!uri)
      {
        *host = '\0';
        return (HTTP_URI_STATUS_BAD_HOSTNAME);
      }

      // Validate value...
      if (*uri != ']')
      {
        *host = '\0';
        return (HTTP_URI_STATUS_BAD_HOSTNAME);
      }

      uri ++;

      for (ptr = host; *ptr; ptr ++)
      {
        if (*ptr == '+')
	{
	  // Convert zone separator to % and stop here...
	  *ptr = '%';
	  break;
	}
	else if (*ptr == '%')
	{
	  // Stop at zone separator (RFC 6874)
	  break;
	}
	else if (*ptr != ':' && *ptr != '.' && !isxdigit(*ptr & 255))
	{
	  *host = '\0';
	  return (HTTP_URI_STATUS_BAD_HOSTNAME);
	}
      }
    }
    else
    {
      // Validate the hostname or IPv4 address first...
      for (ptr = (char *)uri; *ptr; ptr ++)
      {
        if (strchr(":?/", *ptr))
	  break;
        else if (!strchr("abcdefghijklmnopqrstuvwxyz"	// unreserved
			 "ABCDEFGHIJKLMNOPQRSTUVWXYZ"	// unreserved
			 "0123456789"			// unreserved
	        	 "-._~"				// unreserved
			 "%"				// pct-encoded
			 "!$&'()*+,;="			// sub-delims
			 "\\", *ptr))			// SMB domain
	{
	  *host = '\0';
	  return (HTTP_URI_STATUS_BAD_HOSTNAME);
	}
      }

      // Then copy the hostname or IPv4 address to the buffer...
      uri = http_copy_decode(host, uri, (size_t)hostlen, ":?/", decoding & HTTP_URI_CODING_HOSTNAME);

      if (!uri)
      {
        *host = '\0';
        return (HTTP_URI_STATUS_BAD_HOSTNAME);
      }
    }

    // Validate hostname for file scheme - only empty and localhost are
    // acceptable.
    if (!strcmp(scheme, "file") && strcmp(host, "localhost") && host[0])
    {
      *host = '\0';
      return (HTTP_URI_STATUS_BAD_HOSTNAME);
    }

    // See if we have a port number...
    if (*uri == ':')
    {
      // Yes, collect the port number...
      if (!isdigit(uri[1] & 255))
      {
        *port = 0;
        return (HTTP_URI_STATUS_BAD_PORT);
      }

      *port = (int)strtol(uri + 1, (char **)&uri, 10);

      if (*port <= 0 || *port > 65535)
      {
        *port = 0;
        return (HTTP_URI_STATUS_BAD_PORT);
      }

      if (*uri != '/' && *uri)
      {
        *port = 0;
        return (HTTP_URI_STATUS_BAD_PORT);
      }
    }
  }

  // The remaining portion is the resource string...
  if (*uri == '?' || !*uri)
  {
    // Hostname but no path...
    status    = HTTP_URI_STATUS_MISSING_RESOURCE;
    *resource = '/';

    // Copy any query string...
    if (*uri == '?')
      uri = http_copy_decode(resource + 1, uri, (size_t)resourcelen - 1, NULL, decoding & HTTP_URI_CODING_QUERY);
    else
      resource[1] = '\0';
  }
  else
  {
    uri = http_copy_decode(resource, uri, (size_t)resourcelen, "?", decoding & HTTP_URI_CODING_RESOURCE);

    if (uri && *uri == '?')
    {
      // Concatenate any query string...
      char *resptr = resource + strlen(resource);

      uri = http_copy_decode(resptr, uri, (size_t)(resourcelen - (resptr - resource)), NULL, decoding & HTTP_URI_CODING_QUERY);
    }
  }

  if (!uri)
  {
    *resource = '\0';
    return (HTTP_URI_STATUS_BAD_RESOURCE);
  }

  // Return the URI separation status...
  return (status);
}


//
// '_httpSetDigestAuthString()' - Calculate a Digest authentication response using the appropriate RFC 2068/2617/7616 algorithm.
//

int					// O - 1 on success, 0 on failure
_httpSetDigestAuthString(
    http_t     *http,			// I - HTTP connection
    const char *nonce,			// I - Nonce value
    const char *method,			// I - HTTP method
    const char *resource)		// I - HTTP resource path
{
  char		kd[65],			// Final MD5/SHA-256 digest
		ha1[65],		// Hash of username:realm:password
		ha2[65],		// Hash of method:request-uri
		username[HTTP_MAX_VALUE],
					// username:password
		*password,		// Pointer to password
		temp[1024],		// Temporary string
		digest[1024];		// Digest auth data
  unsigned char	hash[32];		// Hash buffer
  size_t	hashsize;		// Size of hash
  _cups_globals_t *cg = _cupsGlobals();	// Per-thread globals


  DEBUG_printf("2_httpSetDigestAuthString(http=%p, nonce=\"%s\", method=\"%s\", resource=\"%s\", qop=\"%s\")", (void *)http, nonce, method, resource, http->qop);

  if (nonce && *nonce && strcmp(nonce, http->nonce))
  {
    cupsCopyString(http->nonce, nonce, sizeof(http->nonce));

    if (nonce == http->nextnonce)
      http->nextnonce[0] = '\0';

    http->nonce_count = 1;
  }
  else
  {
    http->nonce_count ++;
  }

  cupsCopyString(username, http->userpass, sizeof(username));
  if ((password = strchr(username, ':')) != NULL)
    *password++ = '\0';
  else
    return (0);

  if (http->qop[0])
  {
    // Follow RFC 2617/7616...
    int		i;			/* Looping var */
    char	cnonce[65];		/* cnonce value */
    const char	*hashalg;		/* Hashing algorithm */
    const char	*qop;			/* Quality of Protection */

    DEBUG_puts("3_httpSetDigestAuthString: Follow RFC 2617/7616...");

    for (i = 0; i < 64; i ++)
      cnonce[i] = "0123456789ABCDEF"[CUPS_RAND() & 15];
    cnonce[64] = '\0';

    if (!_cups_strcasecmp(http->qop, "auth"))
    {
     /*
      * RFC 2617: "auth" | "auth-int" | token
      */

      qop = "auth";
    }
    else
    {
     /*
      * Some other qop we don't support, skip this one...
      */

      return (0);
    }

    if (!http->algorithm[0] || !_cups_strcasecmp(http->algorithm, "MD5"))
    {
      // RFC 2617 Digest with MD5
      if (cg->digestoptions == _CUPS_DIGESTOPTIONS_DENYMD5)
      {
	DEBUG_puts("3_httpSetDigestAuthString: MD5 Digest is disabled.");
	return (0);
      }

      hashalg = "md5";
    }
    else if (!_cups_strcasecmp(http->algorithm, "SHA-256"))
    {
      // RFC 7616 Digest with SHA-256
      hashalg = "sha2-256";
    }
    else
    {
      // Some other algorithm we don't support, skip this one...
      return (0);
    }

    // Calculate digest value...

    // H(A1) = H(username:realm:password)
    snprintf(temp, sizeof(temp), "%s:%s:%s", username, http->realm, password);
    hashsize = (size_t)cupsHashData(hashalg, (unsigned char *)temp, strlen(temp), hash, sizeof(hash));
    cupsHashString(hash, hashsize, ha1, sizeof(ha1));

    // H(A2) = H(method:uri)
    snprintf(temp, sizeof(temp), "%s:%s", method, resource);
    hashsize = (size_t)cupsHashData(hashalg, (unsigned char *)temp, strlen(temp), hash, sizeof(hash));
    cupsHashString(hash, hashsize, ha2, sizeof(ha2));

    // KD = H(H(A1):nonce:nc:cnonce:qop:H(A2))
    snprintf(temp, sizeof(temp), "%s:%s:%08x:%s:%s:%s", ha1, http->nonce, http->nonce_count, cnonce, qop, ha2);
    hashsize = (size_t)cupsHashData(hashalg, (unsigned char *)temp, strlen(temp), hash, sizeof(hash));
    cupsHashString(hash, hashsize, kd, sizeof(kd));

    // Pass the RFC 2617/7616 WWW-Authenticate header...
    if (http->opaque[0])
      snprintf(digest, sizeof(digest), "username=\"%s\", realm=\"%s\", nonce=\"%s\", algorithm=%s, qop=%s, opaque=\"%s\", cnonce=\"%s\", nc=%08x, uri=\"%s\", response=\"%s\"", cupsUser(), http->realm, http->nonce, http->algorithm, qop, http->opaque, cnonce, http->nonce_count, resource, kd);
    else
      snprintf(digest, sizeof(digest), "username=\"%s\", realm=\"%s\", nonce=\"%s\", algorithm=%s, qop=%s, cnonce=\"%s\", nc=%08x, uri=\"%s\", response=\"%s\"", username, http->realm, http->nonce, http->algorithm, qop, cnonce, http->nonce_count, resource, kd);
  }
  else
  {
    // Use old RFC 2069 Digest method...
    if (cg->digestoptions == _CUPS_DIGESTOPTIONS_DENYMD5)
    {
      DEBUG_puts("3_httpSetDigestAuthString: MD5 Digest is disabled.");
      return (0);
    }

    DEBUG_puts("3_httpSetDigestAuthString: Use old RFC 2069 Digest method...");

    // H(A1) = H(username:realm:password)
    snprintf(temp, sizeof(temp), "%s:%s:%s", username, http->realm, password);
    hashsize = (size_t)cupsHashData("md5", (unsigned char *)temp, strlen(temp), hash, sizeof(hash));
    cupsHashString(hash, hashsize, ha1, sizeof(ha1));

    // H(A2) = H(method:uri)
    snprintf(temp, sizeof(temp), "%s:%s", method, resource);
    hashsize = (size_t)cupsHashData("md5", (unsigned char *)temp, strlen(temp), hash, sizeof(hash));
    cupsHashString(hash, hashsize, ha2, sizeof(ha2));

    // KD = H(H(A1):nonce:H(A2))
    snprintf(temp, sizeof(temp), "%s:%s:%s", ha1, http->nonce, ha2);
    hashsize = (size_t)cupsHashData("md5", (unsigned char *)temp, strlen(temp), hash, sizeof(hash));
    cupsHashString(hash, hashsize, kd, sizeof(kd));

    // Pass the old RFC 2069 WWW-Authenticate header...
    snprintf(digest, sizeof(digest), "username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", response=\"%s\"", username, http->realm, http->nonce, resource, kd);
  }

  httpSetAuthString(http, "Digest", digest);

  return (1);
}


//
// 'httpStateString()' - Return the string describing a HTTP state value.
//
// @since CUPS 2.0/OS 10.10@
//

const char *				// O - State string
httpStateString(http_state_t state)	// I - HTTP state value
{
  if (state < HTTP_STATE_ERROR || state > HTTP_STATE_UNKNOWN_VERSION)
    return ("???");
  else
    return (http_states[state - HTTP_STATE_ERROR]);
}


//
// '_httpStatusString()' - Return the localized string describing a HTTP status code.
//
// The returned string is localized using the passed message catalog.
//

const char *				// O - Localized status string
_httpStatusString(cups_lang_t   *lang,	// I - Language
                  http_status_t status)	// I - HTTP status code
{
  const char	*s;			// Status string


  switch (status)
  {
    case HTTP_STATUS_ERROR :
        s = strerror(errno);
        break;
    case HTTP_STATUS_CONTINUE :
        s = _("Continue");
	break;
    case HTTP_STATUS_SWITCHING_PROTOCOLS :
        s = _("Switching Protocols");
	break;
    case HTTP_STATUS_OK :
        s = _("OK");
	break;
    case HTTP_STATUS_CREATED :
        s = _("Created");
	break;
    case HTTP_STATUS_ACCEPTED :
        s = _("Accepted");
	break;
    case HTTP_STATUS_NO_CONTENT :
        s = _("No Content");
	break;
    case HTTP_STATUS_MOVED_PERMANENTLY :
        s = _("Moved Permanently");
	break;
    case HTTP_STATUS_FOUND :
        s = _("Found");
	break;
    case HTTP_STATUS_SEE_OTHER :
        s = _("See Other");
	break;
    case HTTP_STATUS_NOT_MODIFIED :
        s = _("Not Modified");
	break;
    case HTTP_STATUS_BAD_REQUEST :
        s = _("Bad Request");
	break;
    case HTTP_STATUS_UNAUTHORIZED :
    case HTTP_STATUS_CUPS_AUTHORIZATION_CANCELED :
        s = _("Unauthorized");
	break;
    case HTTP_STATUS_FORBIDDEN :
        s = _("Forbidden");
	break;
    case HTTP_STATUS_NOT_FOUND :
        s = _("Not Found");
	break;
    case HTTP_STATUS_REQUEST_TOO_LARGE :
        s = _("Request Entity Too Large");
	break;
    case HTTP_STATUS_URI_TOO_LONG :
        s = _("URI Too Long");
	break;
    case HTTP_STATUS_UPGRADE_REQUIRED :
        s = _("Upgrade Required");
	break;
    case HTTP_STATUS_NOT_IMPLEMENTED :
        s = _("Not Implemented");
	break;
    case HTTP_STATUS_NOT_SUPPORTED :
        s = _("Not Supported");
	break;
    case HTTP_STATUS_EXPECTATION_FAILED :
        s = _("Expectation Failed");
	break;
    case HTTP_STATUS_SERVICE_UNAVAILABLE :
        s = _("Service Unavailable");
	break;
    case HTTP_STATUS_SERVER_ERROR :
        s = _("Internal Server Error");
	break;
    case HTTP_STATUS_CUPS_PKI_ERROR :
        s = _("SSL/TLS Negotiation Error");
	break;
    case HTTP_STATUS_CUPS_WEBIF_DISABLED :
        s = _("Web Interface is Disabled");
	break;

    default :
        s = _("Unknown");
	break;
  }

  return (_cupsLangString(lang, s));
}


//
// 'httpStatus()' - Return a short string describing a HTTP status code.
//
// @deprecated@ @exclude all@
//

const char *				// O - Localized status string
httpStatus(http_status_t status)	// I - HTTP status code
{
  return (httpStatusString(status));
}


//
// 'httpStatusString()' - Return a short string describing a HTTP status code.
//
// The returned string is localized to the current POSIX locale and is based
// on the status strings defined in RFC 7231.
//
// @since CUPS 2.5@
//

const char *				// O - Localized status string
httpStatusString(http_status_t status)	// I - HTTP status code
{
  _cups_globals_t *cg = _cupsGlobals();	// Global data


  if (!cg->lang_default)
    cg->lang_default = cupsLangDefault();

  return (_httpStatusString(cg->lang_default, status));
}

//
// 'httpURIStatusString()' - Return a string describing a URI status code.
//
// @since CUPS 2.0/OS 10.10@
//

const char *				// O - Localized status string
httpURIStatusString(
    http_uri_status_t status)		// I - URI status code
{
  const char	*s;			// Status string
  _cups_globals_t *cg = _cupsGlobals();	// Global data


  if (!cg->lang_default)
    cg->lang_default = cupsLangDefault();

  switch (status)
  {
    case HTTP_URI_STATUS_OVERFLOW :
	s = _("URI too large");
	break;
    case HTTP_URI_STATUS_BAD_ARGUMENTS :
	s = _("Bad arguments to function");
	break;
    case HTTP_URI_STATUS_BAD_RESOURCE :
	s = _("Bad resource in URI");
	break;
    case HTTP_URI_STATUS_BAD_PORT :
	s = _("Bad port number in URI");
	break;
    case HTTP_URI_STATUS_BAD_HOSTNAME :
	s = _("Bad hostname/address in URI");
	break;
    case HTTP_URI_STATUS_BAD_USERNAME :
	s = _("Bad username in URI");
	break;
    case HTTP_URI_STATUS_BAD_SCHEME :
	s = _("Bad scheme in URI");
	break;
    case HTTP_URI_STATUS_BAD_URI :
	s = _("Bad/empty URI");
	break;
    case HTTP_URI_STATUS_OK :
	s = _("OK");
	break;
    case HTTP_URI_STATUS_MISSING_SCHEME :
	s = _("Missing scheme in URI");
	break;
    case HTTP_URI_STATUS_UNKNOWN_SCHEME :
	s = _("Unknown scheme in URI");
	break;
    case HTTP_URI_STATUS_MISSING_RESOURCE :
	s = _("Missing resource in URI");
	break;

    default:
        s = _("Unknown");
	break;
  }

  return (_cupsLangString(cg->lang_default, s));
}


#ifndef HAVE_HSTRERROR
//
// '_cups_hstrerror()' - hstrerror() emulation function for Solaris and others.
//

const char *				// O - Error string
_cups_hstrerror(int error)		// I - Error number
{
  static const char * const errors[] =	// Error strings
		{
		  "OK",
		  "Host not found.",
		  "Try again.",
		  "Unrecoverable lookup error.",
		  "No data associated with name."
		};


  if (error < 0 || error > 4)
    return ("Unknown hostname lookup error.");
  else
    return (errors[error]);
}
#endif // !HAVE_HSTRERROR


//
// '_httpDecodeURI()' - Percent-decode a HTTP request URI.
//

char *					// O - Decoded URI or NULL on error
_httpDecodeURI(char       *dst,		// I - Destination buffer
               const char *src,		// I - Source URI
	       size_t     dstsize)	// I - Size of destination buffer
{
  if (http_copy_decode(dst, src, dstsize, NULL, 1))
    return (dst);
  else
    return (NULL);
}


//
// '_httpEncodeURI()' - Percent-encode a HTTP request URI.
//

char *					// O - Encoded URI
_httpEncodeURI(char       *dst,		// I - Destination buffer
               const char *src,		// I - Source URI
	       size_t     dstsize)	// I - Size of destination buffer
{
  http_copy_encode(dst, src, dst + dstsize - 1, NULL, NULL, 1);
  return (dst);
}


//
// 'httpResolveURI()' - Resolve a DNS-SD URI.
//
// This function resolves a DNS-SD URI of the form
// "scheme://service-instance-name._protocol._tcp.domain/...".  The "options"
// parameter specifies a bitfield of resolution options including:
//
// - `HTTP_RESOLVE_DEFAULT`: Use default options
// - `HTTP_RESOLVE_FQDN`: Resolve the fully-qualified domain name instead of an IP address
// - `HTTP_RESOLVE_FAXOUT`: Resolve the FaxOut service instead of Print (IPP/IPPS)
//
// The "cb" parameter specifies a callback that allows resolution to be
// terminated.  The callback is provided the "cb_data" value and returns a
// `bool` value that is `true` to continue and `false` to stop.  If no callback
// is specified ("cb" is `NULL`), then this function will block up to 90 seconds
// to resolve the specified URI.
//

const char *				// O - Resolved URI
httpResolveURI(
    const char        *uri,		// I - DNS-SD URI
    char              *resolved_uri,	// I - Buffer for resolved URI
    size_t            resolved_size,	// I - Size of URI buffer
    http_resolve_t    options,		// I - Resolve options
    http_resolve_cb_t cb,		// I - Continue callback function
    void              *cb_data)		// I - Context pointer for callback
{
  char			scheme[32],	// URI components...
			userpass[256],
			hostname[1024],
			resource[1024];
  int			port;
#ifdef DEBUG
  http_uri_status_t	status;		// URI decode status
#endif // DEBUG


  DEBUG_printf("httpResolveURI(uri=\"%s\", resolved_uri=%p, resolved_size=" CUPS_LLFMT ", options=0x%x, cb=%p, cb_data=%p)", uri, (void *)resolved_uri, CUPS_LLCAST resolved_size, options, (void *)cb, cb_data);

  // Get the device URI...
#ifdef DEBUG
  if ((status = httpSeparateURI(HTTP_URI_CODING_ALL, uri, scheme, sizeof(scheme), userpass, sizeof(userpass), hostname, sizeof(hostname), &port, resource, sizeof(resource))) < HTTP_URI_STATUS_OK)
#else
  if (httpSeparateURI(HTTP_URI_CODING_ALL, uri, scheme, sizeof(scheme), userpass, sizeof(userpass), hostname, sizeof(hostname), &port, resource, sizeof(resource)) < HTTP_URI_STATUS_OK)
#endif // DEBUG
  {
    DEBUG_printf("2httpResolveURI: httpSeparateURI returned %d.", status);
    DEBUG_puts("1httpResolveURI: Returning NULL");
    return (NULL);
  }

  // Resolve it as needed...
  if (strstr(hostname, "._tcp"))
  {
    time_t		domain_time,	// Domain lookup time, if any
			end_time;	// End time for resolve
    cups_dnssd_t	*dnssd;		// DNS-SD context
    uint32_t		if_index;	// Interface index
    char		name[256],	// Service instance name
			regtype[256],	// Registration type
			domain[256],	// Domain name
			*uuid,		// Pointer to UUID in URI
			*uuidend;	// Pointer to end of UUID in URI
    _http_uribuf_t	uribuf;		// URI buffer

    // Separate the hostname into service name, registration type, and domain...
    if (!cupsDNSSDSeparateFullName(hostname, name, sizeof(name), regtype, sizeof(regtype), domain, sizeof(domain)))
    {
      DEBUG_puts("2httpResolveURI: Bad hostname, returning NULL");
      return (NULL);
    }

    if ((uuid = strstr(resource, "?uuid=")) != NULL)
    {
      *uuid = '\0';
      uuid  += 6;
      if ((uuidend = strchr(uuid, '&')) != NULL)
        *uuidend = '\0';
    }

    resolved_uri[0] = '\0';

    uribuf.buffer   = resolved_uri;
    uribuf.bufsize  = resolved_size;
    uribuf.options  = options;
    uribuf.resource = resource;
    uribuf.uuid     = uuid;

    DEBUG_printf("2httpResolveURI: Resolving name=\"%s\", regtype=\"%s\",  domain=\"%s\"\n", name, regtype, domain);

    uri = NULL;

    if (!strcmp(scheme, "ippusb"))
      if_index = CUPS_DNSSD_IF_INDEX_LOCAL;
    else
      if_index = CUPS_DNSSD_IF_INDEX_ANY;

    dnssd = cupsDNSSDNew(NULL, NULL);

    if (!cupsDNSSDResolveNew(dnssd, if_index, name, regtype, "local.", http_resolve_cb, &uribuf))
    {
      cupsDNSSDDelete(dnssd);
      return (NULL);
    }

    domain_time = time(NULL) + 2;
    end_time    = time(NULL) + 90;

    while (!resolved_uri[0] && time(NULL) < end_time)
    {
      // Start the domain resolve as needed...
      if (time(NULL) >= domain_time && _cups_strcasecmp(domain, "local."))
      {
	cupsDNSSDResolveNew(dnssd, if_index, name, regtype, domain, http_resolve_cb, &uribuf);
	domain_time = end_time;
      }

      // Sleep 1/4 second to allow time for resolve...
      usleep(250000);

      if (resolved_uri[0])
        break;
    }

    cupsDNSSDDelete(dnssd);

    // Save the results of the resolve...
    uri = *resolved_uri ? resolved_uri : NULL;
  }
  else
  {
    // Nothing more to do...
    cupsCopyString(resolved_uri, uri, resolved_size);
    uri = resolved_uri;
  }

  DEBUG_printf("2httpResolveURI: Returning \"%s\"", uri);

  return (uri);
}


//
// 'http_copy_decode()' - Copy and decode a URI.
//

static const char *			// O - New source pointer or NULL on error
http_copy_decode(char       *dst,	// O - Destination buffer
                 const char *src,	// I - Source pointer
		 size_t     dstsize,	// I - Destination size
		 const char *term,	// I - Terminating characters
		 int        decode)	// I - Decode %-encoded values
{
  char	*ptr,				// Pointer into buffer
	*end;				// End of buffer
  int	quoted;				// Quoted character


 /*
  * Copy the src to the destination until we hit a terminating character
  * or the end of the string.
  */

  for (ptr = dst, end = dst + dstsize - 1;
       *src && (!term || !strchr(term, *src));
       src ++)
    if (ptr < end)
    {
      if (*src == '%' && decode)
      {
        if (isxdigit(src[1] & 255) && isxdigit(src[2] & 255))
	{
	 /*
	  * Grab a hex-encoded character...
	  */

          src ++;
	  if (isalpha(*src))
	    quoted = (tolower(*src) - 'a' + 10) << 4;
	  else
	    quoted = (*src - '0') << 4;

          src ++;
	  if (isalpha(*src))
	    quoted |= tolower(*src) - 'a' + 10;
	  else
	    quoted |= *src - '0';

          *ptr++ = (char)quoted;
	}
	else
	{
	 /*
	  * Bad hex-encoded character...
	  */

	  *ptr = '\0';
	  return (NULL);
	}
      }
      else if ((*src & 255) <= 0x20 || (*src & 255) >= 0x7f)
      {
        *ptr = '\0';
        return (NULL);
      }
      else
	*ptr++ = *src;
    }

  *ptr = '\0';

  return (src);
}


//
// 'http_copy_encode()' - Copy and encode a URI.
//

static char *				// O - End of current URI
http_copy_encode(char       *dst,	// O - Destination buffer
                 const char *src,	// I - Source pointer
		 char       *dstend,	// I - End of destination buffer
                 const char *reserved,	// I - Extra reserved characters
		 const char *term,	// I - Terminating characters
		 int        encode)	// I - %-encode reserved chars?
{
  static const char hex[] = "0123456789ABCDEF";


  while (*src && dst < dstend)
  {
    if (term && *src == *term)
      return (dst);

    if (encode && (*src == '%' || *src <= ' ' || *src & 128 ||
                   (reserved && strchr(reserved, *src))))
    {
     /*
      * Hex encode reserved characters...
      */

      if ((dst + 2) >= dstend)
        break;

      *dst++ = '%';
      *dst++ = hex[(*src >> 4) & 15];
      *dst++ = hex[*src & 15];

      src ++;
    }
    else
      *dst++ = *src++;
  }

  *dst = '\0';

  if (*src)
    return (NULL);
  else
    return (dst);
}


//
// 'http_resolve_cb()' - Build a device URI for the given service name.
//

static void
http_resolve_cb(
    cups_dnssd_resolve_t *res,		// I - Resolver
    void                 *cb_data,	// I - Pointer to URI buffer
    cups_dnssd_flags_t   flags,		// I - Results flags
    uint32_t             if_index,	// I - Interface index
    const char           *fullname,	// I - Full service name
    const char           *host,		// I - Hostname
    uint16_t             port,		// I - Port number
    int                  num_txt,	// I - Number of TXT key/value pairs
    cups_option_t        *txt)		// I - TXT key/value pairs
{
  _http_uribuf_t	*uribuf = (_http_uribuf_t *)cb_data;
					// URI buffer
  const char		*scheme,	// URI scheme
			*hostptr,	// Pointer into hostTarget
			*reskey,	// "rp" or "rfo"
			*resdefault;	// Default path
  char			fqdn[256];	// FQDN of the .local name
  const char		*value,		// Value from TXT record
			*resource;	// Resource path


  DEBUG_printf("4http_resolve_cb(res=%p, cb_data=%p, flags=%x, if_index=%u, fullname=\"%s\", host=\"%s\", port=%u, num_txt=%u, txt=%p)", (void *)res, cb_data, flags, if_index, fullname, host, port, (unsigned)num_txt, (void *)txt);

  // If we have a UUID, compare it...
  if (uribuf->uuid && (value = cupsGetOption("UUID", num_txt, txt)) != NULL)
  {
    if (_cups_strcasecmp(value, uribuf->uuid))
    {
      DEBUG_printf("5http_resolve_cb: Found UUID %s, looking for %s.", value, uribuf->uuid);
      return;
    }
  }

  // Figure out the scheme from the full name...
  if (strstr(fullname, "._ipps") || strstr(fullname, "._ipp-tls"))
    scheme = "ipps";
  else if (strstr(fullname, "._ipp") || strstr(fullname, "._fax-ipp"))
    scheme = "ipp";
  else if (strstr(fullname, "._http."))
    scheme = "http";
  else if (strstr(fullname, "._https."))
    scheme = "https";
  else if (strstr(fullname, "._printer."))
    scheme = "lpd";
  else if (strstr(fullname, "._pdl-datastream."))
    scheme = "socket";
  else
    scheme = "riousbprint";

  // Extract the "remote printer" key from the TXT record...
  if ((uribuf->options & HTTP_RESOLVE_FAXOUT) && (!strcmp(scheme, "ipp") || !strcmp(scheme, "ipps")) && !cupsGetOption("printer-type", num_txt, txt))
  {
    reskey     = "rfo";
    resdefault = "ipp/faxout";
  }
  else
  {
    reskey     = "rp";
    resdefault = "";
  }

  if ((resource = cupsGetOption(reskey, num_txt, txt)) != NULL)
  {
    // Use the resource path from the TXT record...
    if (*resource == '/')
    {
      // Value (incorrectly) has a leading slash already...
      resource ++;
    }
  }
  else
  {
    // Use the default resource path...
    resource = resdefault;
  }

  // Lookup the FQDN if needed...
  if ((uribuf->options & HTTP_RESOLVE_FQDN) && (hostptr = host + strlen(host) - 7) > host && !_cups_strcasecmp(hostptr, ".local."))
  {
    // OK, we got a .local name but the caller needs a real domain.  Start by
    // getting the IP address of the .local name and then do reverse-lookups...
    http_addrlist_t	*addrlist,	// List of addresses
			*addr;		// Current address

    DEBUG_printf("5http_resolve_cb: Looking up \"%s\".", host);

    snprintf(fqdn, sizeof(fqdn), "%d", ntohs(port));
    if ((addrlist = httpAddrGetList(host, AF_UNSPEC, fqdn)) != NULL)
    {
      for (addr = addrlist; addr; addr = addr->next)
      {
        int error = getnameinfo(&(addr->addr.addr), (socklen_t)httpAddrGetLength(&(addr->addr)), fqdn, sizeof(fqdn), NULL, 0, NI_NAMEREQD);

        if (!error)
	{
	  DEBUG_printf("5http_resolve_cb: Found \"%s\".", fqdn);

	  if ((hostptr = fqdn + strlen(fqdn) - 6) <= fqdn || _cups_strcasecmp(hostptr, ".local"))
	  {
	    host = fqdn;
	    break;
	  }
	}
#ifdef DEBUG
	else
	  DEBUG_printf("5http_resolve_cb: \"%s\" did not resolve: %d", httpAddrGetString(&(addr->addr), fqdn, sizeof(fqdn)), error);
#endif // DEBUG
      }

      httpAddrFreeList(addrlist);
    }
  }

  // Assemble the final URI...
  if ((!strcmp(scheme, "ipp") || !strcmp(scheme, "ipps")) && !strcmp(uribuf->resource, "/cups"))
    httpAssembleURIf(HTTP_URI_CODING_ALL, uribuf->buffer, uribuf->bufsize, scheme, NULL, host, port, "/%s?snmp=false", resource);
  else
    httpAssembleURIf(HTTP_URI_CODING_ALL, uribuf->buffer, uribuf->bufsize, scheme, NULL, host, port, "/%s", resource);

  DEBUG_printf("5http_resolve_cb: Resolved URI is \"%s\"...", uribuf->buffer);
}

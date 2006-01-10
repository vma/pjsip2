/* $Id$ */
/* 
 * Copyright (C) 2003-2006 Benny Prijono <benny@prijono.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA 
 */
#include <pjsip/sip_parser.h>
#include <pjsip/sip_uri.h>
#include <pjsip/sip_msg.h>
#include <pjsip/sip_auth_parser.h>
#include <pjsip/sip_errno.h>
#include <pjsip/sip_transport.h>        /* rdata structure */
#include <pjlib-util/scanner.h>
#include <pjlib-util/string.h>
#include <pj/except.h>
#include <pj/log.h>
#include <pj/hash.h>
#include <pj/os.h>
#include <pj/pool.h>
#include <pj/string.h>
#include <pj/ctype.h>
#include <pj/assert.h>

#define ALNUM
#define RESERVED	    ";/?:@&=+$,"
#define MARK		    "-_.!~*'()"
#define UNRESERVED	    ALNUM MARK
#define ESCAPED		    "%"
#define USER_UNRESERVED	    "&=+$,;?/"
#define PASS		    "&=+$,"
#define TOKEN		    "-.!%*_=`'~+"   /* '+' is because of app/pidf+xml
					     * in Content-Type! */
#define HOST		    "_-."
#define HEX_DIGIT	    "abcdefABCDEF"
#define PARAM_CHAR	    "[]/:&+$" UNRESERVED ESCAPED
#define HNV_UNRESERVED	    "[]/?:+$"
#define HDR_CHAR	    HNV_UNRESERVED UNRESERVED ESCAPED

#define PJSIP_VERSION		"SIP/2.0"

#define UNREACHED(expr)

#define IS_NEWLINE(c)	((c)=='\r' || (c)=='\n')
#define IS_SPACE(c)	((c)==' ' || (c)=='\t')

/*
 * Header parser records.
 */
typedef struct handler_rec
{
    char		  hname[PJSIP_MAX_HNAME_LEN+1];
    pj_size_t		  hname_len;
    pj_uint32_t		  hname_hash;
    pjsip_parse_hdr_func *handler;
} handler_rec;

static handler_rec handler[PJSIP_MAX_HEADER_TYPES];
static unsigned handler_count;
static int parser_is_initialized;

/*
 * URI parser records.
 */
typedef struct uri_parser_rec
{
    pj_str_t		     scheme;
    pjsip_parse_uri_func    *parse;
} uri_parser_rec;

static uri_parser_rec uri_handler[PJSIP_MAX_URI_TYPES];
static unsigned uri_handler_count;

/*
 * Global vars (also extern).
 */
int PJSIP_SYN_ERR_EXCEPTION;

const pj_str_t  pjsip_USER_STR      = { "user", 4};
const pj_str_t  pjsip_METHOD_STR    = { "method", 6};
const pj_str_t  pjsip_TRANSPORT_STR = { "transport", 9};
const pj_str_t  pjsip_MADDR_STR     = { "maddr", 5 };
const pj_str_t  pjsip_LR_STR        = { "lr", 2 };
const pj_str_t  pjsip_SIP_STR       = { "sip", 3 };
const pj_str_t  pjsip_SIPS_STR      = { "sips", 4 };
const pj_str_t  pjsip_TEL_STR       = { "tel", 3 };
const pj_str_t  pjsip_BRANCH_STR    = { "branch", 6 };
const pj_str_t  pjsip_TTL_STR       = { "ttl", 3 };
const pj_str_t  pjsip_PNAME_STR     = { "received", 8 };
const pj_str_t  pjsip_Q_STR         = { "q", 1 };
const pj_str_t  pjsip_EXPIRES_STR   = { "expires", 7 };
const pj_str_t  pjsip_TAG_STR       = { "tag", 3 };
const pj_str_t  pjsip_RPORT_STR     = { "rport", 5};

/* Character Input Specification buffer. */
static pj_cis_buf_t cis_buf;

/* Character Input Specifications. */
pj_cis_t    pjsip_HOST_SPEC,	        /* For scanning host part. */
	    pjsip_DIGIT_SPEC,	        /* Decimal digits */
	    pjsip_ALPHA_SPEC,	        /* Alpha (A-Z, a-z) */
	    pjsip_ALNUM_SPEC,	        /* Decimal + Alpha. */
	    pjsip_TOKEN_SPEC,	        /* Token. */
	    pjsip_HEX_SPEC,	        /* Hexadecimal digits. */
	    pjsip_PARAM_CHAR_SPEC,      /* For scanning pname (or pvalue when
                                         * it's not quoted.) */
	    pjsip_HDR_CHAR_SPEC,	/* Chars in hname or hvalue */
	    pjsip_PROBE_USER_HOST_SPEC, /* Hostname characters. */
	    pjsip_PASSWD_SPEC,	        /* Password. */
	    pjsip_USER_SPEC,	        /* User */
	    pjsip_NOT_COMMA_OR_NEWLINE, /* Array separator. */
	    pjsip_NOT_NEWLINE,		/* For eating up header.*/
	    pjsip_DISPLAY_SPEC;         /* Used when searching for display name
                                         * in URL. */


/*
 * Forward decl.
 */
static pjsip_msg *  int_parse_msg( pjsip_parse_ctx *ctx, 
				   pjsip_parser_err_report *err_list);
static void	    int_parse_param( pj_scanner *scanner, 
				     pj_pool_t *pool,
				     pj_str_t *pname, 
				     pj_str_t *pvalue);
static void	    int_parse_hparam( pj_scanner *scanner,
				      pj_pool_t *pool,
				      pj_str_t *hname,
				      pj_str_t *hvalue );
static void         int_parse_req_line( pj_scanner *scanner, 
					pj_pool_t *pool,
					pjsip_request_line *req_line);
static int          int_is_next_user( pj_scanner *scanner);
static void	    int_parse_status_line( pj_scanner *scanner, 
					   pjsip_status_line *line);
static void	    int_parse_user_pass( pj_scanner *scanner, 
					 pj_pool_t *pool,
					 pj_str_t *user, 
					 pj_str_t *pass);
static void	    int_parse_uri_host_port( pj_scanner *scanner, 
					     pj_str_t *p_host, 
					     int *p_port);
static pjsip_uri *  int_parse_uri_or_name_addr( pj_scanner *scanner, 
					        pj_pool_t *pool, 
                                                unsigned option);
static pjsip_sip_uri* int_parse_sip_url( pj_scanner *scanner, 
				         pj_pool_t *pool,
				         pj_bool_t parse_params);
static pjsip_name_addr *
                    int_parse_name_addr( pj_scanner *scanner, 
					 pj_pool_t *pool );
static void	    parse_hdr_end( pj_scanner *scanner );

static pjsip_hdr*   parse_hdr_accept( pjsip_parse_ctx *ctx );
static pjsip_hdr*   parse_hdr_allow( pjsip_parse_ctx *ctx );
static pjsip_hdr*   parse_hdr_call_id( pjsip_parse_ctx *ctx);
static pjsip_hdr*   parse_hdr_contact( pjsip_parse_ctx *ctx);
static pjsip_hdr*   parse_hdr_content_len( pjsip_parse_ctx *ctx );
static pjsip_hdr*   parse_hdr_content_type( pjsip_parse_ctx *ctx );
static pjsip_hdr*   parse_hdr_cseq( pjsip_parse_ctx *ctx );
static pjsip_hdr*   parse_hdr_expires( pjsip_parse_ctx *ctx );
static pjsip_hdr*   parse_hdr_from( pjsip_parse_ctx *ctx );
static pjsip_hdr*   parse_hdr_max_forwards( pjsip_parse_ctx *ctx);
static pjsip_hdr*   parse_hdr_min_expires( pjsip_parse_ctx *ctx );
static pjsip_hdr*   parse_hdr_rr( pjsip_parse_ctx *ctx );
static pjsip_hdr*   parse_hdr_route( pjsip_parse_ctx *ctx );
static pjsip_hdr*   parse_hdr_require( pjsip_parse_ctx *ctx );
static pjsip_hdr*   parse_hdr_retry_after( pjsip_parse_ctx *ctx );
static pjsip_hdr*   parse_hdr_supported( pjsip_parse_ctx *ctx );
static pjsip_hdr*   parse_hdr_to( pjsip_parse_ctx *ctx );
static pjsip_hdr*   parse_hdr_unsupported( pjsip_parse_ctx *ctx );
static pjsip_hdr*   parse_hdr_via( pjsip_parse_ctx *ctx );
static pjsip_hdr*   parse_hdr_generic_string( pjsip_parse_ctx *ctx);

/* Convert non NULL terminated string to integer. */
static unsigned long pj_strtoul_mindigit(const pj_str_t *str, 
                                         unsigned mindig)
{
    unsigned long value;
    unsigned i;

    value = 0;
    for (i=0; i<(unsigned)str->slen; ++i) {
	value = value * 10 + (str->ptr[i] - '0');
    }
    for (; i<mindig; ++i) {
	value = value * 10;
    }
    return value;
}

/* Case insensitive comparison */
#define parser_stricmp(s1, s2)  (pj_stricmp_alnum(&s1, &s2))


/* Syntax error handler for parser. */
static void on_syntax_error(pj_scanner *scanner)
{
    PJ_UNUSED_ARG(scanner);
    PJ_THROW(PJSIP_SYN_ERR_EXCEPTION);
}

/* Concatenate unrecognized params into single string. */
void pjsip_concat_param_imp( pj_str_t *param, pj_pool_t *pool, 
			     const pj_str_t *pname, const pj_str_t *pvalue, 
                             int sepchar)
{
    char *new_param, *p;
    int len;

    len = param->slen + pname->slen + pvalue->slen + 3;
    p = new_param = pj_pool_alloc(pool, len);
    
    if (param->slen) {
	int old_len = param->slen;
	pj_memcpy(p, param->ptr, old_len);
	p += old_len;
    }
    *p++ = (char)sepchar;
    pj_memcpy(p, pname->ptr, pname->slen);
    p += pname->slen;

    if (pvalue->slen) {
	*p++ = '=';
	pj_memcpy(p, pvalue->ptr, pvalue->slen);
	p += pvalue->slen;
    }

    *p = '\0';
    
    param->ptr = new_param;
    param->slen = p - new_param;
}

/* Concatenate unrecognized params into single string. */
static void concat_param( pj_str_t *param, pj_pool_t *pool, 
			  const pj_str_t *pname, const pj_str_t *pvalue )
{
    pjsip_concat_param_imp(param, pool, pname, pvalue, ';');
}

/* Initialize static properties of the parser. */
static pj_status_t init_parser()
{
    static int initialized;
    pj_status_t status;

    if (initialized)
	return PJ_SUCCESS;

    initialized = 1;

    /*
     * Syntax error exception number.
     */
    status = pj_exception_id_alloc("PJSIP: syntax error", 
				   &PJSIP_SYN_ERR_EXCEPTION);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

    /*
     * Init character input spec (cis)
     */

    pj_cis_buf_init(&cis_buf);

    status = pj_cis_init(&cis_buf, &pjsip_DIGIT_SPEC);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);
    pj_cis_add_num(&pjsip_DIGIT_SPEC);
    
    status = pj_cis_init(&cis_buf, &pjsip_ALPHA_SPEC);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);
    pj_cis_add_alpha( &pjsip_ALPHA_SPEC );
    
    status = pj_cis_init(&cis_buf, &pjsip_ALNUM_SPEC);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);
    pj_cis_add_alpha( &pjsip_ALNUM_SPEC );
    pj_cis_add_num( &pjsip_ALNUM_SPEC );

    status = pj_cis_init(&cis_buf, &pjsip_NOT_NEWLINE);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);
    pj_cis_add_str(&pjsip_NOT_NEWLINE, "\r\n");
    pj_cis_invert(&pjsip_NOT_NEWLINE);

    status = pj_cis_init(&cis_buf, &pjsip_NOT_COMMA_OR_NEWLINE);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);
    pj_cis_add_str( &pjsip_NOT_COMMA_OR_NEWLINE, ",\r\n");
    pj_cis_invert(&pjsip_NOT_COMMA_OR_NEWLINE);

    status = pj_cis_dup(&pjsip_TOKEN_SPEC, &pjsip_ALNUM_SPEC);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);
    pj_cis_add_str( &pjsip_TOKEN_SPEC, TOKEN);

    status = pj_cis_dup(&pjsip_HOST_SPEC, &pjsip_ALNUM_SPEC);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);
    pj_cis_add_str( &pjsip_HOST_SPEC, HOST);

    status = pj_cis_dup(&pjsip_HEX_SPEC, &pjsip_DIGIT_SPEC);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);
    pj_cis_add_str( &pjsip_HEX_SPEC, HEX_DIGIT);

    status = pj_cis_dup(&pjsip_PARAM_CHAR_SPEC, &pjsip_ALNUM_SPEC);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);
    pj_cis_add_str(&pjsip_PARAM_CHAR_SPEC, PARAM_CHAR);

    status = pj_cis_dup(&pjsip_HDR_CHAR_SPEC, &pjsip_ALNUM_SPEC);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);
    pj_cis_add_str(&pjsip_HDR_CHAR_SPEC, HDR_CHAR);

    status = pj_cis_dup(&pjsip_USER_SPEC, &pjsip_ALNUM_SPEC);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);
    pj_cis_add_str( &pjsip_USER_SPEC, UNRESERVED ESCAPED USER_UNRESERVED );

    status = pj_cis_dup(&pjsip_PASSWD_SPEC, &pjsip_ALNUM_SPEC);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);
    pj_cis_add_str( &pjsip_PASSWD_SPEC, UNRESERVED ESCAPED PASS);

    status = pj_cis_init(&cis_buf, &pjsip_PROBE_USER_HOST_SPEC);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);
    pj_cis_add_str( &pjsip_PROBE_USER_HOST_SPEC, "@ \n>");
    pj_cis_invert( &pjsip_PROBE_USER_HOST_SPEC );

    status = pj_cis_init(&cis_buf, &pjsip_DISPLAY_SPEC);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);
    pj_cis_add_str( &pjsip_DISPLAY_SPEC, ":\r\n<");
    pj_cis_invert(&pjsip_DISPLAY_SPEC);

    /*
     * Register URI parsers.
     */

    status = pjsip_register_uri_parser("sip", &int_parse_sip_url);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

    status = pjsip_register_uri_parser("sips", &int_parse_sip_url);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

    /*
     * Register header parsers.
     */

    status = pjsip_register_hdr_parser( "Accept", NULL, &parse_hdr_accept);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

    status = pjsip_register_hdr_parser( "Allow", NULL, &parse_hdr_allow);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

    status = pjsip_register_hdr_parser( "Call-ID", NULL, &parse_hdr_call_id);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

    status = pjsip_register_hdr_parser( "Contact", "m", &parse_hdr_contact);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

    status = pjsip_register_hdr_parser( "Content-Length", NULL, 
                                        &parse_hdr_content_len);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

    status = pjsip_register_hdr_parser( "Content-Type", NULL, 
                                        &parse_hdr_content_type);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

    status = pjsip_register_hdr_parser( "CSeq", NULL, &parse_hdr_cseq);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

    status = pjsip_register_hdr_parser( "Expires", NULL, &parse_hdr_expires);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

    status = pjsip_register_hdr_parser( "From", "f", &parse_hdr_from);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

    status = pjsip_register_hdr_parser( "Max-Forwards", NULL, 
                                        &parse_hdr_max_forwards);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

    status = pjsip_register_hdr_parser( "Min-Expires", NULL, 
                                        &parse_hdr_min_expires);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

    status = pjsip_register_hdr_parser( "Record-Route", NULL, &parse_hdr_rr);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

    status = pjsip_register_hdr_parser( "Route", NULL, &parse_hdr_route);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

    status = pjsip_register_hdr_parser( "Require", NULL, &parse_hdr_require);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

    status = pjsip_register_hdr_parser( "Retry-After", NULL, 
                                        &parse_hdr_retry_after);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

    status = pjsip_register_hdr_parser( "Supported", "k", 
                                        &parse_hdr_supported);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

    status = pjsip_register_hdr_parser( "To", "t", &parse_hdr_to);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

    status = pjsip_register_hdr_parser( "Unsupported", NULL, 
                                        &parse_hdr_unsupported);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

    status = pjsip_register_hdr_parser( "Via", NULL, &parse_hdr_via);
    PJ_ASSERT_RETURN(status == PJ_SUCCESS, status);

    /* 
     * Register auth parser. 
     */

    status = pjsip_auth_init_parser();

    return status;
}

void init_sip_parser(void)
{
    if (!parser_is_initialized) {
	if (!parser_is_initialized) {
	    init_parser();
	    parser_is_initialized = 1;
	}
    }
}

/* Compare the handler record with header name, and return:
 * - 0  if handler match.
 * - <0 if handler is 'less' than the header name.
 * - >0 if handler is 'greater' than header name.
 */
PJ_INLINE(int) compare_handler( const handler_rec *r1, 
				const char *name, 
				pj_size_t name_len,
				pj_uint32_t hash )
{
    PJ_UNUSED_ARG(name_len);

    /* Compare hashed value. */
    if (r1->hname_hash < hash)
	return -1;
    if (r1->hname_hash > hash)
	return 1;

    /* Compare length. */
    /*
    if (r1->hname_len < name_len)
	return -1;
    if (r1->hname_len > name_len)
	return 1;
     */

    /* Equal length and equal hash. compare the strings. */
    return pj_native_strcmp(r1->hname, name);
}

/* Register one handler for one header name. */
static pj_status_t int_register_parser( const char *name, 
                                        pjsip_parse_hdr_func *fptr )
{
    unsigned	pos;
    handler_rec rec;
    unsigned	i;

    if (handler_count >= PJ_ARRAY_SIZE(handler)) {
	return PJ_ETOOMANY;
    }

    /* Initialize temporary handler. */
    rec.handler = fptr;
    rec.hname_len = strlen(name);
    if (rec.hname_len >= sizeof(rec.hname)) {
	return PJ_ENAMETOOLONG;
    }
    /* Name is copied in lowercase. */
    for (i=0; i<rec.hname_len; ++i) {
	rec.hname[i] = (char)pj_tolower(name[i]);
    }
    rec.hname[i] = '\0';
    /* Hash value is calculated from the lowercase name. */
    rec.hname_hash = pj_hash_calc(0, rec.hname, PJ_HASH_KEY_STRING);

    /* Get the pos to insert the new handler. */
    for (pos=0; pos < handler_count; ++pos) {
	int d;
	d = compare_handler(&handler[pos], rec.hname, rec.hname_len, 
                            rec.hname_hash);
	if (d == 0) {
	    pj_assert(0);
	    return PJ_EEXISTS;
	}
	if (d > 0) {
	    break;
	}
    }

    /* Shift handlers. */
    if (pos != handler_count) {
	pj_memmove( &handler[pos+1], &handler[pos], 
                    (handler_count-pos)*sizeof(handler_rec));
    }
    /* Add new handler. */
    pj_memcpy( &handler[pos], &rec, sizeof(handler_rec));
    ++handler_count;

    return PJ_SUCCESS;
}

/* Register parser handler. If both header name and short name are valid,
 * then two instances of handler will be registered.
 */
PJ_DEF(pj_status_t) pjsip_register_hdr_parser( const char *hname,
					       const char *hshortname,
					       pjsip_parse_hdr_func *fptr)
{
    pj_status_t status;

    status = int_register_parser(hname, fptr);
    if (status != PJ_SUCCESS) {
	return status;
    }
    if (hshortname) {
        status = int_register_parser(hshortname, fptr);
        if (status != PJ_SUCCESS) 
	    return status;
    }
    return PJ_SUCCESS;
}

/* Find handler to parse the header name. */
static pjsip_parse_hdr_func * find_handler(const pj_str_t *hname)
{
    handler_rec *first;
    char	 hname_copy[PJSIP_MAX_HNAME_LEN];
    pj_uint32_t  hash;
    int		 comp;
    unsigned	 n;

    if (hname->slen >= PJSIP_MAX_HNAME_LEN) {
        pj_assert(!"Header name is too long!");
        return NULL;
    }

    /* Calculate hash value while converting the header to lowercase. 
     * Don't assume that 'hname' is NULL terminated.
     */
    hash = pj_hash_calc_tolower(0, hname_copy, hname);
    hname_copy[hname->slen] = '\0';

    /* Binary search for the handler. */
    comp = -1;
    first = &handler[0];
    n = handler_count;
    for (; n > 0; ) {
	unsigned half = n / 2;
	handler_rec *mid = first + half;

	comp = compare_handler(mid, hname_copy, hname->slen, hash);
	if (comp < 0) {
	    first = ++mid;
	    n -= half + 1;
	} else if (comp==0) {
	    first = mid;
	    break;
	} else {
	    n = half;
	}
    }

    return comp==0 ? first->handler : NULL;
}

/* Find URI handler. */
static pjsip_parse_uri_func* find_uri_handler(const pj_str_t *scheme)
{
    unsigned i;
    for (i=0; i<uri_handler_count; ++i) {
	if (pj_stricmp_alnum(&uri_handler[i].scheme, scheme)==0)
	    return uri_handler[i].parse;
    }
    return NULL;
}

/* Register URI parser. */
PJ_DEF(pj_status_t) pjsip_register_uri_parser( char *scheme,
					       pjsip_parse_uri_func *func)
{
    if (uri_handler_count >= PJ_ARRAY_SIZE(uri_handler))
	return PJ_ETOOMANY;

    uri_handler[uri_handler_count].scheme = pj_str((char*)scheme);
    uri_handler[uri_handler_count].parse = func;
    ++uri_handler_count;

    return PJ_SUCCESS;
}

/* Public function to parse SIP message. */
PJ_DEF(pjsip_msg*) pjsip_parse_msg( pj_pool_t *pool, 
                                    char *buf, pj_size_t size,
				    pjsip_parser_err_report *err_list)
{
    pjsip_msg *msg = NULL;
    pj_scanner scanner;
    pjsip_parse_ctx context;

    pj_scan_init(&scanner, buf, size, PJ_SCAN_AUTOSKIP_WS_HEADER, 
                 &on_syntax_error);

    context.scanner = &scanner;
    context.pool = pool;
    context.rdata = NULL;

    msg = int_parse_msg(&context, err_list);

    pj_scan_fini(&scanner);
    return msg;
}

/* Public function to parse as rdata.*/
PJ_DEF(pjsip_msg *) pjsip_parse_rdata( char *buf, pj_size_t size,
                                       pjsip_rx_data *rdata )
{
    pj_scanner scanner;
    pjsip_parse_ctx context;

    pj_scan_init(&scanner, buf, size, PJ_SCAN_AUTOSKIP_WS_HEADER, 
                 &on_syntax_error);

    context.scanner = &scanner;
    context.pool = rdata->tp_info.pool;
    context.rdata = rdata;

    rdata->msg_info.msg = int_parse_msg(&context, &rdata->msg_info.parse_err);

    pj_scan_fini(&scanner);
    return rdata->msg_info.msg;
}

/* Determine if a message has been received. */
PJ_DEF(pj_bool_t) pjsip_find_msg( const char *buf, pj_size_t size, 
				  pj_bool_t is_datagram, pj_size_t *msg_size)
{
#if PJ_HAS_TCP
    const char *hdr_end;
    const char *body_start;
    const char *pos;
    const char *line;
    int content_length = -1;

    *msg_size = size;

    /* For datagram, the whole datagram IS the message. */
    if (is_datagram) {
	return PJ_SUCCESS;
    }


    /* Find the end of header area by finding an empty line. */
    pos = pj_native_strstr(buf, "\n\r\n");
    if (pos == NULL) {
	return PJSIP_EPARTIALMSG;
    }
 
    hdr_end = pos+1;
    body_start = pos+3;

    /* Find "Content-Length" header the hard way. */
    line = pj_native_strchr(buf, '\n');
    while (line && line < hdr_end) {
	++line;
	if ( ((*line=='C' || *line=='c') && 
              strnicmp_alnum(line, "Content-Length", 14) == 0) ||
	     ((*line=='l' || *line=='L') && 
              (*(line+1)==' ' || *(line+1)=='\t' || *(line+1)==':')))
	{
	    /* Try to parse the header. */
	    pj_scanner scanner;
	    PJ_USE_EXCEPTION;

	    pj_scan_init(&scanner, (char*)line, hdr_end-line, 
			 PJ_SCAN_AUTOSKIP_WS_HEADER, &on_syntax_error);

	    PJ_TRY {
		pj_str_t str_clen;

		/* Get "Content-Length" or "L" name */
		if (*line=='C' || *line=='c')
		    pj_scan_advance_n(&scanner, 14, PJ_TRUE);
		else if (*line=='l' || *line=='L')
		    pj_scan_advance_n(&scanner, 1, PJ_TRUE);

		/* Get colon */
		if (pj_scan_get_char(&scanner) != ':') {
		    PJ_THROW(PJSIP_SYN_ERR_EXCEPTION);
		}

		/* Get number */
		pj_scan_get(&scanner, &pjsip_DIGIT_SPEC, &str_clen);

		/* Get newline. */
		pj_scan_get_newline(&scanner);

		/* Found a valid Content-Length header. */
		content_length = pj_strtoul(&str_clen);
	    }
	    PJ_CATCH_ANY {
		content_length = -1;
	    }
	    PJ_END

	    pj_scan_fini(&scanner);
	}

	/* Found valid Content-Length? */
	if (content_length != -1)
	    break;

	/* Go to next line. */
	line = pj_native_strchr(line, '\n');
    }

    /* Found Content-Length? */
    if (content_length == -1) {
	return PJSIP_EMISSINGHDR;
    }

    /* Enough packet received? */
    *msg_size = (body_start - buf) + content_length;
    return (*msg_size) <= size ? PJ_SUCCESS : PJSIP_EPARTIALMSG;
#else
    PJ_UNUSED_ARG(buf);
    PJ_UNUSED_ARG(is_datagram);
    *msg_size = size;
    return PJ_SUCCESS;
#endif
}

/* Public function to parse URI */
PJ_DEF(pjsip_uri*) pjsip_parse_uri( pj_pool_t *pool, 
					 char *buf, pj_size_t size,
					 unsigned option)
{
    pj_scanner scanner;
    pjsip_uri *uri = NULL;
    PJ_USE_EXCEPTION;

    pj_scan_init(&scanner, buf, size, 0, &on_syntax_error);

    
    PJ_TRY {
	uri = int_parse_uri_or_name_addr(&scanner, pool, option);
    }
    PJ_CATCH_ANY {
	uri = NULL;
    }
    PJ_END;

    /* Must have exhausted all inputs. */
    if (pj_scan_is_eof(&scanner) || IS_NEWLINE(*scanner.curptr)) {
	/* Success. */
	pj_scan_fini(&scanner);
	return uri;
    }

    /* Still have some characters unparsed. */
    pj_scan_fini(&scanner);
    return NULL;
}

/* Generic function to print message body.
 * This assumes that the 'data' member points to a contigous memory where the 
 * actual body is laid.
 */
static int generic_print_body (pjsip_msg_body *msg_body, 
                               char *buf, pj_size_t size)
{
    pjsip_msg_body *body = msg_body;
    if (size < body->len)
	return 0;

    pj_memcpy (buf, body->data, body->len);
    return body->len;
}

/* Internal function to parse SIP message */
static pjsip_msg *int_parse_msg( pjsip_parse_ctx *ctx,
				 pjsip_parser_err_report *err_list)
{
    pj_bool_t parsing_headers;
    pjsip_msg *msg = NULL;
    pj_str_t hname;
    pjsip_ctype_hdr *ctype_hdr = NULL;
    pj_scanner *scanner = ctx->scanner;
    pj_pool_t *pool = ctx->pool;
    PJ_USE_EXCEPTION;

    parsing_headers = PJ_FALSE;

    PJ_TRY 
    {
	if (parsing_headers)
	    goto parse_headers;

	/* Skip leading newlines. */
	while (IS_NEWLINE(*scanner->curptr)) {
	    pj_scan_get_newline(scanner);
	}

	/* Parse request or status line */
	if (pj_scan_stricmp_alnum( scanner, PJSIP_VERSION, 7) == 0) {
	    msg = pjsip_msg_create(pool, PJSIP_RESPONSE_MSG);
	    int_parse_status_line( scanner, &msg->line.status );
	} else {
	    msg = pjsip_msg_create(pool, PJSIP_REQUEST_MSG);
	    int_parse_req_line(scanner, pool, &msg->line.req );
	}

	parsing_headers = PJ_TRUE;

parse_headers:
	/* Parse headers. */
	do {
	    pjsip_parse_hdr_func * handler;
	    pjsip_hdr *hdr = NULL;
	    
	    /* Get hname. */
	    pj_scan_get( scanner, &pjsip_TOKEN_SPEC, &hname);
	    if (pj_scan_get_char( scanner ) != ':') {
		PJ_THROW(PJSIP_SYN_ERR_EXCEPTION);
	    }
	    
	    /* Find handler. */
	    handler = find_handler(&hname);
	    
	    /* Call the handler if found.
	     * If no handler is found, then treat the header as generic
	     * hname/hvalue pair.
	     */
	    if (handler) {
		hdr = (*handler)(ctx);

		/* Check if we've just parsed a Content-Type header. 
		 * We will check for a message body if we've got Content-Type 
		 * header.
		 */
		if (hdr->type == PJSIP_H_CONTENT_TYPE) {
		    ctype_hdr = (pjsip_ctype_hdr*)hdr;
		}

	    } else {
		hdr = parse_hdr_generic_string(ctx);
		hdr->name = hdr->sname = hname;
	    }
	    
	
	    /* Single parse of header line can produce multiple headers.
	     * For example, if one Contact: header contains Contact list
	     * separated by comma, then these Contacts will be split into
	     * different Contact headers.
	     * So here we must insert list instead of just insert one header.
	     */
	    pj_list_insert_nodes_before(&msg->hdr, hdr);
	    
	    /* Parse until EOF or an empty line is found. */
	} while (!pj_scan_is_eof(scanner) && !IS_NEWLINE(*scanner->curptr));
	
	parsing_headers = PJ_FALSE;

	/* If empty line is found, eat it. */
	if (!pj_scan_is_eof(scanner)) {
	    if (IS_NEWLINE(*scanner->curptr)) {
		pj_scan_get_newline(scanner);
	    }
	}

	/* If we have Content-Type header, treat the rest of the message 
	 * as body.
	 */
	if (ctype_hdr && scanner->curptr!=scanner->end) {
	    pjsip_msg_body *body = pj_pool_alloc(pool, sizeof(pjsip_msg_body));
	    body->content_type.type = ctype_hdr->media.type;
	    body->content_type.subtype = ctype_hdr->media.subtype;
	    body->content_type.param = ctype_hdr->media.param;

	    body->data = scanner->curptr;
	    body->len = scanner->end - scanner->curptr;
	    body->print_body = &generic_print_body;

	    msg->body = body;
	}
    }
    PJ_CATCH_ANY 
    {
	/* Exception was thrown during parsing. 
	 * Skip until newline, and parse next header. 
	 */
	if (err_list) {
	    pjsip_parser_err_report *err_info;
	    
	    err_info = pj_pool_alloc(pool, sizeof(*err_info));
	    err_info->except_code = PJ_GET_EXCEPTION();
	    err_info->line = scanner->line;
	    err_info->col = pj_scan_get_col(scanner);
	    if (parsing_headers)
		err_info->hname = hname;
	    else
		err_info->hname.slen = 0;
	    
	    pj_list_insert_before(err_list, err_info);
	}
	
	if (parsing_headers) {
	    if (!pj_scan_is_eof(scanner)) {
		/* Skip until next line.
		 * Watch for header continuation.
		 */
		do {
		    pj_scan_skip_line(scanner);
		} while (IS_SPACE(*scanner->curptr));
	    }

	    /* Restore flag. Flag may be set in int_parse_sip_url() */
	    scanner->skip_ws = PJ_SCAN_AUTOSKIP_WS_HEADER;

	    /* Continue parse next header, if any. */
	    if (!pj_scan_is_eof(scanner) && !IS_NEWLINE(*scanner->curptr)) {
		goto parse_headers;
	    }
	}

	msg = NULL;
    }
    PJ_END;

    return msg;
}

/* Parse parameter (pname ["=" pvalue]). */
void pjsip_parse_param_imp(  pj_scanner *scanner, pj_pool_t *pool,
			     pj_str_t *pname, pj_str_t *pvalue,
			     unsigned option)
{
    /* pname */
    pj_scan_get(scanner, &pjsip_PARAM_CHAR_SPEC, pname);
    *pname = pj_str_unescape(pool, pname);

    /* init pvalue */
    pvalue->ptr = NULL;
    pvalue->slen = 0;

    /* pvalue, if any */
    if (*scanner->curptr == '=') {
	pj_scan_get_char(scanner);
	if (!pj_scan_is_eof(scanner)) {
	    /* pvalue can be a quoted string. */
	    if (*scanner->curptr == '"') {
		pj_scan_get_quote( scanner, '"', '"', pvalue);
		if (option & PJSIP_PARSE_REMOVE_QUOTE) {
		    pvalue->ptr++;
		    pvalue->slen -= 2;
		}
	    } else if(pj_cis_match(&pjsip_PARAM_CHAR_SPEC, *scanner->curptr)) {
		pj_scan_get(scanner, &pjsip_PARAM_CHAR_SPEC, pvalue);
		*pvalue = pj_str_unescape(pool, pvalue);
	    }
	}
    }
}

/* Parse parameter (";" pname ["=" pvalue]). */
static void int_parse_param( pj_scanner *scanner, pj_pool_t *pool,
			     pj_str_t *pname, pj_str_t *pvalue)
{
    /* Get ';' character */
    pj_scan_get_char(scanner);

    /* Get pname and optionally pvalue */
    pjsip_parse_param_imp(scanner, pool, pname, pvalue, 
			  PJSIP_PARSE_REMOVE_QUOTE);
}

/* Parse header parameter. */
static void int_parse_hparam( pj_scanner *scanner, pj_pool_t *pool,
			      pj_str_t *hname, pj_str_t *hvalue )
{
    /* Get '?' or '&' character. */
    pj_scan_get_char(scanner);

    /* hname */
    pj_scan_get(scanner, &pjsip_HDR_CHAR_SPEC, hname);
    *hname = pj_str_unescape(pool, hname);

    /* Init hvalue */
    hvalue->ptr = NULL;
    hvalue->slen = 0;

    /* pvalue, if any */
    if (*scanner->curptr == '=') {
	pj_scan_get_char(scanner);
	if (!pj_scan_is_eof(scanner) && 
	    pj_cis_match(&pjsip_HDR_CHAR_SPEC, *scanner->curptr))
	{
	    pj_scan_get(scanner, &pjsip_HDR_CHAR_SPEC, hvalue);
	    *hvalue = pj_str_unescape(pool, hvalue);
	}
    }
}

/* Parse host:port in URI. */
static void int_parse_uri_host_port( pj_scanner *scanner, 
				     pj_str_t *host, int *p_port)
{
    pj_scan_get( scanner, &pjsip_HOST_SPEC, host);
    /* RFC3261 section 19.1.2: host don't need to be unescaped */
    if (*scanner->curptr == ':') {
	pj_str_t port;
	pj_scan_get_char(scanner);
	pj_scan_get(scanner, &pjsip_DIGIT_SPEC, &port);
	*p_port = pj_strtoul(&port);
    } else {
	*p_port = 0;
    }
}

/* Determine if the next token in an URI is a user specification. */
static int int_is_next_user(pj_scanner *scanner)
{
    pj_str_t dummy;
    int is_user;

    /* Find character '@'. If this character exist, then the token
     * must be a username.
     */
    if (pj_scan_peek( scanner, &pjsip_PROBE_USER_HOST_SPEC, &dummy) == '@')
	is_user = 1;
    else
	is_user = 0;

    return is_user;
}

/* Parse user:pass tokens in an URI. */
static void int_parse_user_pass( pj_scanner *scanner, pj_pool_t *pool,
				 pj_str_t *user, pj_str_t *pass)
{
    pj_scan_get( scanner, &pjsip_USER_SPEC, user);
    *user = pj_str_unescape(pool, user);

    if ( *scanner->curptr == ':') {
	pj_scan_get_char( scanner );
	pj_scan_get( scanner, &pjsip_PASSWD_SPEC, pass);
	*pass = pj_str_unescape(pool, pass);
    } else {
	pass->ptr = NULL;
	pass->slen = 0;
    }

    /* Get the '@' */
    pj_scan_get_char( scanner );
}

/* Parse all types of URI. */
static pjsip_uri *int_parse_uri_or_name_addr( pj_scanner *scanner, pj_pool_t *pool,
					      unsigned opt)
{
    pjsip_uri *uri;
    int is_name_addr = 0;

    /* Exhaust any whitespaces. */
    pj_scan_skip_whitespace(scanner);

    if (*scanner->curptr=='"' || *scanner->curptr=='<') {
	uri = (pjsip_uri*)int_parse_name_addr( scanner, pool );
	is_name_addr = 1;
    } else {
	pj_str_t scheme;
	int next_ch;

	next_ch = pj_scan_peek( scanner, &pjsip_DISPLAY_SPEC, &scheme);

	if (next_ch==':') {
	    pjsip_parse_uri_func *func = find_uri_handler(&scheme);

	    if (func == NULL) {
		/* Unsupported URI scheme */
		PJ_THROW(PJSIP_SYN_ERR_EXCEPTION);
	    }

	    uri = (*func)( scanner, pool, 
			  (opt & PJSIP_PARSE_URI_IN_FROM_TO_HDR)== 0);


	} else {
	    uri = (pjsip_uri*)int_parse_name_addr( scanner, pool );
	    is_name_addr = 1;
	}
    }

    /* Should we return the URI object as name address? */
    if (opt & PJSIP_PARSE_URI_AS_NAMEADDR) {
	if (is_name_addr == 0) {
	    pjsip_name_addr *name_addr;

	    name_addr = pjsip_name_addr_create(pool);
	    name_addr->uri = uri;

	    uri = (pjsip_uri*)name_addr;
	}
    }

    return uri;
}

/* Parse URI. */
static pjsip_uri *int_parse_uri(pj_scanner *scanner, pj_pool_t *pool, 
				pj_bool_t parse_params)
{
    if (*scanner->curptr=='"' || *scanner->curptr=='<') {
	return (pjsip_uri*)int_parse_name_addr( scanner, pool );
    } else {
	pj_str_t scheme;
	int colon;
	pjsip_parse_uri_func *func;

	/* Get scheme. */
	colon = pj_scan_peek(scanner, &pjsip_TOKEN_SPEC, &scheme);
	if (colon != ':') {
	    PJ_THROW(PJSIP_SYN_ERR_EXCEPTION);
	}

	func = find_uri_handler(&scheme);
	if (func)  {
	    return (pjsip_uri*)(*func)(scanner, pool, parse_params);

	} else {
	    /* Unsupported URI scheme */
	    PJ_THROW(PJSIP_SYN_ERR_EXCEPTION);
	    UNREACHED({ return NULL; /* Not reached. */ })
	}
    }
}

/* Parse "sip:" and "sips:" URI. */
static pjsip_sip_uri *int_parse_sip_url( pj_scanner *scanner, 
					 pj_pool_t *pool,
					 pj_bool_t parse_params)
{
    pj_str_t scheme;
    pjsip_sip_uri *url = NULL;
    int colon;
    int skip_ws = scanner->skip_ws;
    scanner->skip_ws = 0;

    pj_scan_get(scanner, &pjsip_TOKEN_SPEC, &scheme);
    colon = pj_scan_get_char(scanner);
    if (colon != ':') {
	PJ_THROW(PJSIP_SYN_ERR_EXCEPTION);
    }

    if (parser_stricmp(scheme, pjsip_SIP_STR)==0) {
	url = pjsip_url_create(pool, 0);

    } else if (parser_stricmp(scheme, pjsip_SIPS_STR)==0) {
	url = pjsip_url_create(pool, 1);

    } else {
	PJ_THROW(PJSIP_SYN_ERR_EXCEPTION);
	/* should not reach here */
	UNREACHED({
	    pj_assert(0);
	    return 0;
	})
    }

    if (int_is_next_user(scanner)) {
	int_parse_user_pass(scanner, pool, &url->user, &url->passwd);
    }

    /* Get host:port */
    int_parse_uri_host_port(scanner, &url->host, &url->port);

    /* Get URL parameters. */
    if (parse_params) {
      while (*scanner->curptr == ';' ) {
	pj_str_t pname, pvalue;

	int_parse_param( scanner, pool, &pname, &pvalue);

	if (!parser_stricmp(pname, pjsip_USER_STR) && pvalue.slen) {
	    url->user_param = pvalue;

	} else if (!parser_stricmp(pname, pjsip_METHOD_STR) && pvalue.slen) {
	    url->method_param = pvalue;

	} else if (!parser_stricmp(pname,pjsip_TRANSPORT_STR) && pvalue.slen) {
	    url->transport_param = pvalue;

	} else if (!parser_stricmp(pname, pjsip_TTL_STR) && pvalue.slen) {
	    url->ttl_param = pj_strtoul(&pvalue);

	} else if (!parser_stricmp(pname, pjsip_MADDR_STR) && pvalue.slen) {
	    url->maddr_param = pvalue;

	} else if (!parser_stricmp(pname, pjsip_LR_STR)) {
	    url->lr_param = 1;

	} else {
	    pjsip_param *p = pj_pool_alloc(pool, sizeof(pjsip_param));
	    p->name = pname;
	    p->value = pvalue;
	    pj_list_insert_before(&url->other_param, p);
	}
      }
    }

    /* Get header params. */
    if (parse_params && *scanner->curptr == '?') {
      do {
	pjsip_param *param;
	param = pj_pool_alloc(pool, sizeof(pjsip_param));
	int_parse_hparam(scanner, pool, &param->name, &param->value);
	pj_list_insert_before(&url->header_param, param);
      } while (*scanner->curptr == '&');
    }

    scanner->skip_ws = skip_ws;
    pj_scan_skip_whitespace(scanner);
    return url;
}

/* Parse nameaddr. */
static pjsip_name_addr *int_parse_name_addr( pj_scanner *scanner, 
					     pj_pool_t *pool )
{
    int has_bracket;
    pjsip_name_addr *name_addr;

    name_addr = pjsip_name_addr_create(pool);

    if (*scanner->curptr == '"') {
	pj_scan_get_quote( scanner, '"', '"', &name_addr->display);
	/* Trim the leading and ending quote */
	name_addr->display.ptr++;
	name_addr->display.slen -= 2;

    } else if (*scanner->curptr != '<') {
	int next;
	pj_str_t dummy;

	/* This can be either the start of display name,
	 * the start of URL ("sip:", "sips:", "tel:", etc.), or '<' char.
	 * We're only interested in display name, because SIP URL
	 * will be parser later.
	 */
	next = pj_scan_peek(scanner, &pjsip_DISPLAY_SPEC, &dummy);
	if (next == '<') {
	    /* Ok, this is what we're looking for, a display name. */
	    pj_scan_get_until_ch( scanner, '<', &name_addr->display);
	    pj_strtrim(&name_addr->display);
	}
    }

    /* Manually skip whitespace. */
    pj_scan_skip_whitespace(scanner);

    /* Get the SIP-URL */
    has_bracket = (*scanner->curptr == '<');
    if (has_bracket)
	pj_scan_get_char(scanner);
    name_addr->uri = int_parse_uri( scanner, pool, PJ_TRUE );
    if (has_bracket) {
	if (pj_scan_get_char(scanner) != '>')
	    PJ_THROW( PJSIP_SYN_ERR_EXCEPTION);
    }

    return name_addr;
}


/* Parse SIP request line. */
static void int_parse_req_line( pj_scanner *scanner, pj_pool_t *pool,
				pjsip_request_line *req_line)
{
    pj_str_t token;

    pj_scan_get( scanner, &pjsip_TOKEN_SPEC, &token);
    pjsip_method_init_np( &req_line->method, &token);

    req_line->uri = int_parse_uri(scanner, pool, PJ_TRUE);
    if (pj_scan_stricmp_alnum( scanner, PJSIP_VERSION, 7) != 0)
	PJ_THROW( PJSIP_SYN_ERR_EXCEPTION);
    pj_scan_advance_n (scanner, 7, 1);
    pj_scan_get_newline( scanner );
}

/* Parse status line. */
static void int_parse_status_line( pj_scanner *scanner, 
				   pjsip_status_line *status_line)
{
    pj_str_t token;

    if (pj_scan_stricmp_alnum(scanner, PJSIP_VERSION, 7) != 0)
	PJ_THROW(PJSIP_SYN_ERR_EXCEPTION);
    pj_scan_advance_n( scanner, 7, 1);

    pj_scan_get( scanner, &pjsip_DIGIT_SPEC, &token);
    status_line->code = pj_strtoul(&token);
    pj_scan_get( scanner, &pjsip_NOT_NEWLINE, &status_line->reason);
    pj_scan_get_newline( scanner );
}

/* Parse ending of header. */
static void parse_hdr_end( pj_scanner *scanner )
{
    if (pj_scan_is_eof(scanner)) {
	;   /* Do nothing. */
    } else if (*scanner->curptr == '&') {
	pj_scan_get_char(scanner);
    } else {
	pj_scan_get_newline(scanner);
    }
}

/* Parse ending of header. */
void pjsip_parse_end_hdr_imp( pj_scanner *scanner )
{
    parse_hdr_end(scanner);
}

/* Parse generic array header. */
static void parse_generic_array_hdr( pjsip_generic_array_hdr *hdr,
				     pj_scanner *scanner)
{
    pj_scan_get( scanner, &pjsip_NOT_COMMA_OR_NEWLINE, &hdr->values[0]);
    hdr->count++;

    while (*scanner->curptr == ',') {
	pj_scan_get_char(scanner);
	pj_scan_get( scanner, &pjsip_NOT_COMMA_OR_NEWLINE, 
		     &hdr->values[hdr->count]);
	hdr->count++;
    }
    parse_hdr_end(scanner);
}

/* Parse generic string header. */
static void parse_generic_string_hdr( pjsip_generic_string_hdr *hdr,
				      pj_scanner *scanner )
{
    if (pj_cis_match(&pjsip_NOT_NEWLINE, *scanner->curptr))
	pj_scan_get( scanner, &pjsip_NOT_NEWLINE, &hdr->hvalue);
    else
	hdr->hvalue.slen = 0;

    parse_hdr_end(scanner);
}

/* Parse generic integer header. */
static void parse_generic_int_hdr( pjsip_generic_int_hdr *hdr,
				   pj_scanner *scanner )
{
    pj_str_t tmp;
    pj_scan_get( scanner, &pjsip_DIGIT_SPEC, &tmp);
    hdr->ivalue = pj_strtoul(&tmp);
    parse_hdr_end(scanner);
}


/* Parse Accept header. */
static pjsip_hdr* parse_hdr_accept(pjsip_parse_ctx *ctx)
{
    pjsip_accept_hdr *accept = pjsip_accept_hdr_create(ctx->pool);
    parse_generic_array_hdr(accept, ctx->scanner);
    return (pjsip_hdr*)accept;
}

/* Parse Allow header. */
static pjsip_hdr* parse_hdr_allow(pjsip_parse_ctx *ctx)
{
    pjsip_allow_hdr *allow = pjsip_allow_hdr_create(ctx->pool);
    parse_generic_array_hdr(allow, ctx->scanner);
    return (pjsip_hdr*)allow;
}

/* Parse Call-ID header. */
static pjsip_hdr* parse_hdr_call_id(pjsip_parse_ctx *ctx)
{
    pjsip_cid_hdr *hdr = pjsip_cid_hdr_create(ctx->pool);
    pj_scan_get( ctx->scanner, &pjsip_NOT_NEWLINE, &hdr->id);
    parse_hdr_end(ctx->scanner);

    if (ctx->rdata)
        ctx->rdata->msg_info.call_id = hdr->id;

    return (pjsip_hdr*)hdr;
}

/* Parse and interpret Contact param. */
static void int_parse_contact_param( pjsip_contact_hdr *hdr, 
				     pj_scanner *scanner,
				     pj_pool_t *pool)
{
    while ( *scanner->curptr == ';' ) {
	pj_str_t pname, pvalue;

	int_parse_param( scanner, pool, &pname, &pvalue);
	if (!parser_stricmp(pname, pjsip_Q_STR) && pvalue.slen) {
	    char *dot_pos = memchr(pvalue.ptr, '.', pvalue.slen);
	    if (!dot_pos) {
		hdr->q1000 = pj_strtoul(&pvalue);
	    } else {
		pvalue.slen = (pvalue.ptr+pvalue.slen) - (dot_pos+1);
		pvalue.ptr = dot_pos + 1;
		hdr->q1000 = pj_strtoul_mindigit(&pvalue, 3);
	    }    
	} else if (!parser_stricmp(pname, pjsip_EXPIRES_STR) && pvalue.slen) {
	    hdr->expires = pj_strtoul(&pvalue);

	} else {
	    pjsip_param *p = pj_pool_alloc(pool, sizeof(pjsip_param));
	    p->name = pname;
	    p->value = pvalue;
	    pj_list_insert_before(&hdr->other_param, p);
	}
    }
}

/* Parse Contact header. */
static pjsip_hdr* parse_hdr_contact( pjsip_parse_ctx *ctx )
{
    pjsip_contact_hdr *first = NULL;
    pj_scanner *scanner = ctx->scanner;
    
    do {
	pjsip_contact_hdr *hdr = pjsip_contact_hdr_create(ctx->pool);
	if (first == NULL)
	    first = hdr;
	else
	    pj_list_insert_before(first, hdr);

	if (*scanner->curptr == '*') {
	    pj_scan_get_char(scanner);
	    hdr->star = 1;

	} else {
	    hdr->star = 0;
	    hdr->uri = int_parse_uri_or_name_addr(scanner, ctx->pool, 
                                                  PJSIP_PARSE_URI_AS_NAMEADDR |
						  PJSIP_PARSE_URI_IN_FROM_TO_HDR);

	    int_parse_contact_param(hdr, scanner, ctx->pool);
	}

	if (*scanner->curptr != ',')
	    break;

	pj_scan_get_char(scanner);

    } while (1);

    parse_hdr_end(scanner);

    return (pjsip_hdr*)first;
}

/* Parse Content-Length header. */
static pjsip_hdr* parse_hdr_content_len( pjsip_parse_ctx *ctx )
{
    pj_str_t digit;
    pjsip_clen_hdr *hdr;

    hdr = pjsip_clen_hdr_create(ctx->pool);
    pj_scan_get(ctx->scanner, &pjsip_DIGIT_SPEC, &digit);
    hdr->len = pj_strtoul(&digit);
    parse_hdr_end(ctx->scanner);

    if (ctx->rdata)
        ctx->rdata->msg_info.clen = hdr;

    return (pjsip_hdr*)hdr;
}

/* Parse Content-Type header. */
static pjsip_hdr* parse_hdr_content_type( pjsip_parse_ctx *ctx )
{
    pjsip_ctype_hdr *hdr;
    pj_scanner *scanner = ctx->scanner;

    hdr = pjsip_ctype_hdr_create(ctx->pool);
    
    /* Parse media type and subtype. */
    pj_scan_get(scanner, &pjsip_TOKEN_SPEC, &hdr->media.type);
    pj_scan_get_char(scanner);
    pj_scan_get(scanner, &pjsip_TOKEN_SPEC, &hdr->media.subtype);

    /* Parse media parameters */
    while (*scanner->curptr == ';') {
	pj_str_t pname, pvalue;
	int_parse_param(scanner, ctx->pool, &pname, &pvalue);
	concat_param(&hdr->media.param, ctx->pool, &pname, &pvalue);
    }

    parse_hdr_end(ctx->scanner);

    if (ctx->rdata)
        ctx->rdata->msg_info.ctype = hdr;

    return (pjsip_hdr*)hdr;
}

/* Parse CSeq header. */
static pjsip_hdr* parse_hdr_cseq( pjsip_parse_ctx *ctx )
{
    pj_str_t cseq, method;
    pjsip_cseq_hdr *hdr;

    hdr = pjsip_cseq_hdr_create(ctx->pool);
    pj_scan_get( ctx->scanner, &pjsip_DIGIT_SPEC, &cseq);
    hdr->cseq = pj_strtoul(&cseq);

    pj_scan_get( ctx->scanner, &pjsip_TOKEN_SPEC, &method);
    pjsip_method_init_np(&hdr->method, &method);

    parse_hdr_end( ctx->scanner );

    if (ctx->rdata)
        ctx->rdata->msg_info.cseq = hdr;

    return (pjsip_hdr*)hdr;
}

/* Parse Expires header. */
static pjsip_hdr* parse_hdr_expires(pjsip_parse_ctx *ctx)
{
    pjsip_expires_hdr *hdr = pjsip_expires_hdr_create(ctx->pool);
    parse_generic_int_hdr(hdr, ctx->scanner);
    return (pjsip_hdr*)hdr;
}

/* Parse From: or To: header. */
static void parse_hdr_fromto( pj_scanner *scanner, 
			      pj_pool_t *pool, 
			      pjsip_from_hdr *hdr)
{
    hdr->uri = int_parse_uri_or_name_addr(scanner, pool, 
					  PJSIP_PARSE_URI_AS_NAMEADDR |
					  PJSIP_PARSE_URI_IN_FROM_TO_HDR);

    while ( *scanner->curptr == ';' ) {
	pj_str_t pname, pvalue;

	int_parse_param( scanner, pool, &pname, &pvalue);

	if (!parser_stricmp(pname, pjsip_TAG_STR)) {
	    hdr->tag = pvalue;
	    
	} else {
	    pjsip_param *p = pj_pool_alloc(pool, sizeof(pjsip_param));
	    p->name = pname;
	    p->value = pvalue;
	    pj_list_insert_before(&hdr->other_param, p);
	}
    }

    parse_hdr_end(scanner);
}

/* Parse From: header. */
static pjsip_hdr* parse_hdr_from( pjsip_parse_ctx *ctx )
{
    pjsip_from_hdr *hdr = pjsip_from_hdr_create(ctx->pool);
    parse_hdr_fromto(ctx->scanner, ctx->pool, hdr);
    if (ctx->rdata)
        ctx->rdata->msg_info.from = hdr;

    return (pjsip_hdr*)hdr;
}

/* Parse Require: header. */
static pjsip_hdr* parse_hdr_require( pjsip_parse_ctx *ctx )
{
    pjsip_require_hdr *hdr = pjsip_require_hdr_create(ctx->pool);
    parse_generic_array_hdr(hdr, ctx->scanner);

    if (ctx->rdata && ctx->rdata->msg_info.require == NULL)
        ctx->rdata->msg_info.require = hdr;

    return (pjsip_hdr*)hdr;
}

/* Parse Retry-After: header. */
static pjsip_hdr* parse_hdr_retry_after(pjsip_parse_ctx *ctx)
{
    pjsip_retry_after_hdr *hdr;
    hdr = pjsip_retry_after_hdr_create(ctx->pool);
    parse_generic_int_hdr(hdr, ctx->scanner);
    return (pjsip_hdr*)hdr;
}

/* Parse Supported: header. */
static pjsip_hdr* parse_hdr_supported(pjsip_parse_ctx *ctx)
{
    pjsip_supported_hdr *hdr = pjsip_supported_hdr_create(ctx->pool);
    parse_generic_array_hdr(hdr, ctx->scanner);
    return (pjsip_hdr*)hdr;
}


/* Parse To: header. */
static pjsip_hdr* parse_hdr_to( pjsip_parse_ctx *ctx )
{
    pjsip_to_hdr *hdr = pjsip_to_hdr_create(ctx->pool);
    parse_hdr_fromto(ctx->scanner, ctx->pool, hdr);

    if (ctx->rdata)
        ctx->rdata->msg_info.to = hdr;

    return (pjsip_hdr*)hdr;
}

/* Parse Unsupported: header. */
static pjsip_hdr* parse_hdr_unsupported(pjsip_parse_ctx *ctx)
{
    pjsip_unsupported_hdr *hdr = pjsip_unsupported_hdr_create(ctx->pool);
    parse_generic_array_hdr(hdr, ctx->scanner);
    return (pjsip_hdr*)hdr;
}

/* Parse and interpret Via parameters. */
static void int_parse_via_param( pjsip_via_hdr *hdr, pj_scanner *scanner,
				 pj_pool_t *pool)
{
    while ( *scanner->curptr == ';' ) {
	pj_str_t pname, pvalue;

	int_parse_param( scanner, pool, &pname, &pvalue);

	if (!parser_stricmp(pname, pjsip_BRANCH_STR) && pvalue.slen) {
	    hdr->branch_param = pvalue;

	} else if (!parser_stricmp(pname, pjsip_TTL_STR) && pvalue.slen) {
	    hdr->ttl_param = pj_strtoul(&pvalue);
	    
	} else if (!parser_stricmp(pname, pjsip_MADDR_STR) && pvalue.slen) {
	    hdr->maddr_param = pvalue;

	} else if (!parser_stricmp(pname, pjsip_PNAME_STR) && pvalue.slen) {
	    hdr->recvd_param = pvalue;

	} else if (!parser_stricmp(pname, pjsip_RPORT_STR)) {
	    if (pvalue.slen)
		hdr->rport_param = pj_strtoul(&pvalue);
	    else
		hdr->rport_param = 0;
	} else {
	    pjsip_param *p = pj_pool_alloc(pool, sizeof(pjsip_param));
	    p->name = pname;
	    p->value = pvalue;
	    pj_list_insert_before(&hdr->other_param, p);
	}
    }
}

/* Parse Max-Forwards header. */
static pjsip_hdr* parse_hdr_max_forwards( pjsip_parse_ctx *ctx )
{
    pjsip_max_forwards_hdr *hdr;
    hdr = pjsip_max_forwards_hdr_create(ctx->pool);
    parse_generic_int_hdr(hdr, ctx->scanner);

    if (ctx->rdata)
        ctx->rdata->msg_info.max_fwd = hdr;

    return (pjsip_hdr*)hdr;
}

/* Parse Min-Expires header. */
static pjsip_hdr* parse_hdr_min_expires(pjsip_parse_ctx *ctx)
{
    pjsip_min_expires_hdr *hdr;
    hdr = pjsip_min_expires_hdr_create(ctx->pool);
    parse_generic_int_hdr(hdr, ctx->scanner);
    return (pjsip_hdr*)hdr;
}


/* Parse Route: or Record-Route: header. */
static void parse_hdr_rr_route( pj_scanner *scanner, pj_pool_t *pool,
				pjsip_routing_hdr *hdr )
{
    pjsip_name_addr *temp=int_parse_name_addr(scanner, pool);

    pj_memcpy(&hdr->name_addr, temp, sizeof(*temp));

    while (*scanner->curptr == ';') {
	pjsip_param *p = pj_pool_alloc(pool, sizeof(pjsip_param));
	int_parse_param(scanner, pool, &p->name, &p->value);
	pj_list_insert_before(&hdr->other_param, p);
    }
}

/* Parse Record-Route header. */
static pjsip_hdr* parse_hdr_rr( pjsip_parse_ctx *ctx)
{
    pjsip_rr_hdr *first = NULL;
    pj_scanner *scanner = ctx->scanner;

    do {
	pjsip_rr_hdr *hdr = pjsip_rr_hdr_create(ctx->pool);
	if (!first) {
	    first = hdr;
	} else {
	    pj_list_insert_before(first, hdr);
	}
	parse_hdr_rr_route(scanner, ctx->pool, hdr);
	if (*scanner->curptr == ',') {
	    pj_scan_get_char(scanner);
	} else {
	    break;
	}
    } while (1);
    parse_hdr_end(scanner);

    if (ctx->rdata && ctx->rdata->msg_info.record_route==NULL)
        ctx->rdata->msg_info.record_route = first;

    return (pjsip_hdr*)first;
}

/* Parse Route: header. */
static pjsip_hdr* parse_hdr_route( pjsip_parse_ctx *ctx )
{
    pjsip_route_hdr *first = NULL;
    pj_scanner *scanner = ctx->scanner;

    do {
	pjsip_route_hdr *hdr = pjsip_route_hdr_create(ctx->pool);
	if (!first) {
	    first = hdr;
	} else {
	    pj_list_insert_before(first, hdr);
	}
	parse_hdr_rr_route(scanner, ctx->pool, hdr);
	if (*scanner->curptr == ',') {
	    pj_scan_get_char(scanner);
	} else {
	    break;
	}
    } while (1);
    parse_hdr_end(scanner);

    if (ctx->rdata && ctx->rdata->msg_info.route==NULL)
        ctx->rdata->msg_info.route = first;

    return (pjsip_hdr*)first;
}

/* Parse Via: header. */
static pjsip_hdr* parse_hdr_via( pjsip_parse_ctx *ctx )
{
    pjsip_via_hdr *first = NULL;
    pj_scanner *scanner = ctx->scanner;

    do {
	pjsip_via_hdr *hdr = pjsip_via_hdr_create(ctx->pool);
	if (!first)
	    first = hdr;
	else
	    pj_list_insert_before(first, hdr);

	if (pj_scan_stricmp_alnum( scanner, PJSIP_VERSION "/", 8) != 0)
	    PJ_THROW(PJSIP_SYN_ERR_EXCEPTION);

	pj_scan_advance_n( scanner, 8, 1);

	pj_scan_get( scanner, &pjsip_TOKEN_SPEC, &hdr->transport);
	pj_scan_get( scanner, &pjsip_HOST_SPEC, &hdr->sent_by.host);

	if (*scanner->curptr==':') {
	    pj_str_t digit;
	    pj_scan_get_char(scanner);
	    pj_scan_get(scanner, &pjsip_DIGIT_SPEC, &digit);
	    hdr->sent_by.port = pj_strtoul(&digit);
	}
	
	int_parse_via_param(hdr, scanner, ctx->pool);

	if (*scanner->curptr == '(') {
	    pj_scan_get_char(scanner);
	    pj_scan_get_until_ch( scanner, ')', &hdr->comment);
	    pj_scan_get_char( scanner );
	}

	if (*scanner->curptr != ',')
	    break;

	pj_scan_get_char(scanner);

    } while (1);

    parse_hdr_end(scanner);

    if (ctx->rdata && ctx->rdata->msg_info.via == NULL)
        ctx->rdata->msg_info.via = first;

    return (pjsip_hdr*)first;
}

/* Parse generic header. */
static pjsip_hdr* parse_hdr_generic_string( pjsip_parse_ctx *ctx )
{
    pjsip_generic_string_hdr *hdr;

    hdr = pjsip_generic_string_hdr_create(ctx->pool, NULL);
    parse_generic_string_hdr(hdr, ctx->scanner);
    return (pjsip_hdr*)hdr;

}

/* Public function to parse a header value. */
PJ_DEF(void*) pjsip_parse_hdr( pj_pool_t *pool, const pj_str_t *hname,
			       char *buf, pj_size_t size, int *parsed_len )
{
    pj_scanner scanner;
    pjsip_hdr *hdr = NULL;
    pjsip_parse_ctx context;
    PJ_USE_EXCEPTION;

    pj_scan_init(&scanner, buf, size, PJ_SCAN_AUTOSKIP_WS_HEADER, 
                 &on_syntax_error);

    context.scanner = &scanner;
    context.pool = pool;
    context.rdata = NULL;

    PJ_TRY {
	pjsip_parse_hdr_func *handler = find_handler(hname);
	if (handler) {
	    hdr = (*handler)(&context);
	} else {
	    hdr = parse_hdr_generic_string(&context);
	    hdr->type = PJSIP_H_OTHER;
	    pj_strdup(pool, &hdr->name, hname);
	    hdr->sname = hdr->name;
	}

    } 
    PJ_CATCH_ANY {
	hdr = NULL;
    }
    PJ_END

    if (parsed_len) {
	*parsed_len = (scanner.curptr - scanner.begin);
    }

    pj_scan_fini(&scanner);

    return hdr;
}


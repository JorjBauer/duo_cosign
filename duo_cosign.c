/*
 * Copyright (c) 2013 Andrew Mortensen
 * All rights reserved. See LICENSE.
 */

#include <sys/types.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <curl/curl.h>

#include "duo_cosign_api.h"
#include "duo_cosign_cfg.h"
#include "duo_cosign_curl.h"
#include "duo_cosign_json.h"

#define DC_EXEC_NAME_AUTH	"duo_cosign"
#define DC_EXEC_NAME_AUTH_STAT	"duo_cosign_auth_status"
#define DC_EXEC_NAME_CHECK	"duo_cosign_check"
#define DC_EXEC_NAME_ENROLL	"duo_cosign_enroll"
#define DC_EXEC_NAME_PING	"duo_cosign_ping"
#define DC_EXEC_NAME_PREAUTH	"duo_cosign_preauth"


extern int		errno;

char			*xname;

    static char *
dc_get_exec_name( char *exec_path )
{
    char		*exec_name;

    exec_name = strrchr( exec_path, '/' );
    if ( exec_name != NULL ) {
	exec_name++;
	if ( *exec_name == '\0' ) {
	    abort();
	}
    } else {
	exec_name = exec_path;
    }

    return( exec_name );
}

    static char *
dc_read_input_line( void )
{
    char		buf[ 512 ];
    char		*line;
    int			len;

    if ( fgets( buf, sizeof( buf ), stdin ) == NULL ) {
	fprintf( stderr, "fgets failed: %s\n", strerror( errno ));
	exit( 2 );
    }

    len = strlen( buf );
    if ( buf[ len - 1 ] != '\n' ) {
	fprintf( stderr, "fgets failed: line too long\n" );
	exit( 2 );
    }
    buf[ len - 1 ] = '\0';

    if (( line = strdup( buf )) == NULL ) {
	fprintf( stderr, "strdup failed: %s\n", strerror( errno ));
	exit( 2 );
    }

    return( line );
}

enum {
    DC_EXEC_MODE_DEFAULT = 0,
    DC_EXEC_MODE_USERFACTOR = (1 << 0),
};
#define DC_EXEC_MODE_PREAUTH_DEFAULT \
	(DC_EXEC_MODE_DEFAULT | DC_EXEC_MODE_USERFACTOR)

    static int
dc_exec_ping( int argc, char **argv, dc_cfg_entry_t *cfg, int flags )
{
    time_t		tstamp;

    if ( dc_ping( cfg, &tstamp ) != DC_STATUS_OK ) {
	fprintf( stderr, "ping failed\n" );
	exit( 2 );
    }

    printf( "%ld\n", tstamp );

    return( DC_STATUS_OK );
}

    static int
dc_exec_check( int argc, char **argv, dc_cfg_entry_t *cfg, int flags )
{
    time_t		tstamp;

    if ( dc_check( cfg, &tstamp ) != DC_STATUS_OK ) {
	fprintf( stderr, "check failed\n" );
	exit( 2 );
    }

    printf( "%ld\n", tstamp );

    return( DC_STATUS_OK );
}

    static int
dc_exec_preauth( int argc, char **argv, dc_cfg_entry_t *cfg, int flags )
{
    dc_preauth_result_t	presult;
    char		*user;
    char		*factor_name;
    char		*device_json;
    int			rc = 0;

    assert( argc >= 1 );
    assert( argv != NULL );

    user = argv[ 0 ];

    switch ( dc_preauth( cfg, user, &presult )) {
    case DC_STATUS_AUTH_REQUIRED:
	device_json = dc_device_list_json_serialize( presult.devices );
	if ( device_json == NULL ) {
	    fprintf( stderr, "%s: failed to JSON serialize device list\n",
			xname );
	    printf( "Access denied\n" );
	    rc = 1;
	    break;
	}

	/* emit device list as a variable */
	printf( "$duo_devices_json=%s\n", device_json );

	/* json_dumps returns a malloc'd string */
	free( device_json );

	/* if we're running as userfactor check, indicate factor's required */
	if (( flags & DC_EXEC_MODE_USERFACTOR )) {
	    factor_name = DC_CFG_FACTOR_NAME( cfg );
	    printf( "%s\n", factor_name ? factor_name : _DC_FACTOR_NAME );
	}
	break;

    case DC_STATUS_USER_ALLOWED:
	/* user is configured to bypass 2f, no stdout output */
	fprintf( stderr, "%s: user %s configured to bypass 2f\n", xname, user );
	break;

    case DC_STATUS_USER_DENIED:
	/* print error message, exit non-zero */
	printf( "Access denied\n" );
	rc = 1;
	break;

    case DC_STATUS_USER_NOT_ENROLLED:
	/* XXX add config support for auto-enrollment and prompt to enroll */
	fprintf( stderr, "%s: user %s not enrolled\n", xname, user );
	break;

    default:
	printf( "Access denied\n" );
	rc = 1;
	break;
    }

    return( rc );
}

    int
dc_exec_auth( int argc, char **argv, dc_cfg_entry_t *cfg, int flags )
{
    dc_auth_t		auth;
    dc_auth_result_t	aresult;
    char		*factor_name;
    int			rc = 0;

    memset( &auth, 0, sizeof( dc_auth_t ));
    auth.user = dc_read_input_line();
    auth.factor = dc_read_input_line();
    auth.data = dc_read_input_line();

    switch ( dc_auth( cfg, &auth, &aresult )) {
    case DC_STATUS_USER_ALLOWED:
	factor_name = DC_CFG_FACTOR_NAME( cfg );
	printf( "%s\n", factor_name ? factor_name : _DC_FACTOR_NAME );
	break;

    case DC_STATUS_AUTH_PENDING:
	/* async auth, ensure we exit non-zero so the cgi loads the template */
	rc = 1;

	if ( aresult.txid == NULL ) {
	    fprintf( stderr, "%s: ERROR: pending authentication for user %s, "
			"but no txid returned by auth request\n",
			xname, auth.user );
	    printf( "Authentication failed\n" );
	    break;
	}

	printf( "$duo_auth_type=%s\n", auth.factor );
	printf( "$duo_txid=%s\n", aresult.txid );
	printf( "Authentication pending\n" );
	break;

    default:
	printf( "Authentication failed\n" );
	rc = 1;

	fprintf( stderr, "%s: %s authentication failed for user %s: %s (%s)\n",
		xname, auth.factor, auth.user, aresult.status_msg,
		aresult.status );

	/*
	 * authentication failed in some way. run preauth again to
	 * re-populate device list, but make sure we don't run it
	 * as a userfactor, since that will emit the factor name,
	 * which the cgi will take as a successful authentication.
	 */
	dc_exec_preauth( 1, (char **)&auth.user, cfg, DC_EXEC_MODE_DEFAULT );

	break;
    }

    free( auth.user );
    free( auth.factor );
    free( auth.data );

    return( rc );
}

    int
main( int ac, char *av[] )
{
    dc_cfg_entry_t	*cfg_list = NULL;
    dc_param_t		*params = NULL;
    dc_response_t	resp;
    dc_status_t		rc = DC_STATUS_FAIL;
    char		*cfg_path;
    char		*user = NULL;
    char		*auth_type = NULL;
    char		*auth_data = NULL;
    int			status;
    int			i;
    struct {
	const char	*exec_name;
	int		(*exec_fn)( int, char **, dc_cfg_entry_t *, int );
	int		exec_flags;
    } exec_name_tab[] = {
	{ DC_EXEC_NAME_AUTH, dc_exec_auth, DC_EXEC_MODE_DEFAULT },
	{ DC_EXEC_NAME_CHECK, dc_exec_check, DC_EXEC_MODE_DEFAULT },
	{ DC_EXEC_NAME_PING, dc_exec_ping, DC_EXEC_MODE_DEFAULT },
	{ DC_EXEC_NAME_PREAUTH, dc_exec_preauth, DC_EXEC_MODE_PREAUTH_DEFAULT },
	{ NULL, NULL },
    };

    xname = dc_get_exec_name( av[ 0 ] );

    /* advance past exec name so exec functions receive only arguments */
    ac--;
    av++;

    cfg_path = dc_get_cfg_path();
    status = dc_cfg_read( cfg_path, &cfg_list );
    if ( status < 0 ) {
	fprintf( stderr, "duo_cosign: failed to read config %s\n", cfg_path );
	exit( 2 );
    }

    if ( curl_global_init( CURL_GLOBAL_ALL ) != 0 ) {
	fprintf( stderr, "curl_global_init failed\n" );
	exit( 2 );
    }

    for ( i = 0; exec_name_tab[ i ].exec_name != NULL; i++ ) {
	if ( strcmp( xname, exec_name_tab[ i ].exec_name ) == 0 ) {
	    break;
	}
    }
    if ( exec_name_tab[ i ].exec_name == NULL ) {
	fprintf( stderr, "%s: unrecognized execution name\n", xname );
	exit( 1 );
    }

    rc = exec_name_tab[ i ].exec_fn( ac, av, cfg_list,
					exec_name_tab[ i ].exec_flags );
    
#ifdef notdef
    if ( strcmp( xname, "duo_cosign_preauth" ) == 0 ) {
	dc_exec_preauth( cfg_list );
    } else if ( strcmp( xname, "duo_cosign" ) == 0 ) {
	user = dc_read_input_line();
	auth_type = dc_read_input_line();
	auth_data = dc_read_input_line();

	DC_PARAMS_ADD( &params, USERNAME, user );
	DC_PARAMS_ADD( &params, FACTOR, auth_type );
	if ( strcmp( auth_type, "passcode" ) == 0 ) {
	    DC_PARAMS_ADD( &params, PASSCODE, auth_data );
	} else if ( strcmp( auth_type, "phone" ) == 0 ) {
	    DC_PARAMS_ADD( &params, DEVICE, auth_data );
	} else if ( strcmp( auth_type, "push" ) == 0 ) {
	    DC_PARAMS_ADD( &params, DEVICE, auth_data );
	}
	dc_api_request_dispatch( DC_AUTH_URL_REF_ID, params,
		cfg_list, &resp );
	dc_param_list_free( &params );

    }
#endif /* notdef */

    curl_global_cleanup();

    return( rc );
}

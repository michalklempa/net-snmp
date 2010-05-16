#include <net-snmp/net-snmp-config.h>

#if defined(NETSNMP_USE_OPENSSL) && defined(HAVE_LIBSSL)

#include <ctype.h>
#if HAVE_MALLOC_H
# include <malloc.h>
#endif
#if HAVE_SYS_STAT_H
#   include <sys/stat.h>
#endif
#if HAVE_DIRENT_H
# include <dirent.h>
# define NAMLEN(dirent) strlen((dirent)->d_name)
#else
# define dirent direct
# define NAMLEN(dirent) (dirent)->d_namlen
# if HAVE_SYS_NDIR_H
#  include <sys/ndir.h>
# endif
# if HAVE_SYS_DIR_H
#  include <sys/dir.h>
# endif
# if HAVE_NDIR_H
#  include <ndir.h>
# endif
#endif

#if HAVE_DMALLOC_H
#include <dmalloc.h>
#endif

#include <net-snmp/types.h>
#include <net-snmp/output_api.h>
#include <net-snmp/config_api.h>

#include <net-snmp/library/snmp_assert.h>
#include <net-snmp/library/snmp_transport.h>
#include <net-snmp/library/system.h>
#include <net-snmp/library/tools.h>
#include <net-snmp/library/container.h>
#include <net-snmp/library/data_list.h>
#include <net-snmp/library/file_utils.h>
#include <net-snmp/library/dir_utils.h>
#include <net-snmp/library/read_config.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <net-snmp/library/cert_util.h>
#include <net-snmp/library/snmp_openssl.h>

/*
 * bump this value whenever cert index format changes, so indexes
 * will be reginerated with new format.
 */
#define CERT_INDEX_FORMAT  1

static netsnmp_container *_certs = NULL;
static netsnmp_container *_keys = NULL;
static netsnmp_container *_maps = NULL;
static struct snmp_enum_list *_certindexes = NULL;

static void _setup_containers(void);

static void _cert_indexes_load(void);
static void _cert_free(netsnmp_cert *cert, void *context);
static void _key_free(netsnmp_key *key, void *context);
static int  _cert_compare(netsnmp_cert *lhs, netsnmp_cert *rhs);
static int  _cert_sn_compare(netsnmp_cert *lhs, netsnmp_cert *rhs);
static int  _cert_sn_ncompare(netsnmp_cert *lhs, netsnmp_cert *rhs);
static int  _cert_cn_compare(netsnmp_cert *lhs, netsnmp_cert *rhs);
static int  _cert_fn_compare(netsnmp_cert_common *lhs,
                             netsnmp_cert_common *rhs);
static int  _cert_fn_ncompare(netsnmp_cert_common *lhs,
                              netsnmp_cert_common *rhs);
static void _find_partner(netsnmp_cert *cert, netsnmp_key *key);
static netsnmp_cert *_find_issuer(netsnmp_cert *cert);
static netsnmp_void_array *_cert_find_subset_fn(const char *filename,
                                                const char *directory);
static netsnmp_void_array *_cert_find_subset_sn(const char *subject);
static netsnmp_void_array *_key_find_subset(const char *filename);
static netsnmp_cert *_cert_find_fp(const char *fingerprint);
static const char *_mode_str(u_char mode);
static const char *_where_str(u_int what);
void netsnmp_cert_dump_all(void);

int netsnmp_cert_load_x509(netsnmp_cert *cert);

void netsnmp_cert_free(netsnmp_cert *cert);
void netsnmp_key_free(netsnmp_key *key);

static int _certindex_add( const char *dirname, int i );

static int _time_filter(netsnmp_file *f, struct stat *idx);

static void _parse_map(const char *token, char *line);
static int _save_maps(int majorID, int minorID, void *server, void *client);
static int _save_map(netsnmp_cert_map *map, void *type);

/** mode descriptions should match up with header */
static const char _modes[][256] =
        {
            "none",
            "identity",
            "remote_peer",
            "identity+remote_peer",
            "reserved1",
            "reserved1+identity",
            "reserved1+remote_peer",
            "reserved1+identity+remote_peer",
            "CA",
            "CA+identity",
            "CA+remote_peer",
            "CA+identity+remote_peer",
            "CA+reserved1",
            "CA+reserved1+identity",
            "CA+reserved1+remote_peer",
            "CA+reserved1+identity+remote_peer",
        };

/* #####################################################################
 *
 * init and shutdown functions
 *
 */
void
netsnmp_certs_init(void)
{
    const char *certSecName_help = "certSecName PRIORITY FINGERPRINT "
        "[--shaNN|md5] <--sn SECNAME | --rfc822 | --dns | --ip | --cn | --any>";

    char *app = netsnmp_ds_get_string(NETSNMP_DS_LIBRARY_ID,
                                      NETSNMP_DS_LIB_APPTYPE);

    /*
     * secname mapping only makes sense for servers.
     * Is there a better way than apptype to determine that?
     */
    if ((strcmp(app, "snmpd") == 0) || (strcmp(app, "snmptrapd") == 0)) {
        register_config_handler(NULL, "certSecName", _parse_map, NULL,
                                certSecName_help);
        if (snmp_register_callback(SNMP_CALLBACK_LIBRARY,
                                   SNMP_CALLBACK_STORE_DATA,
                                   _save_maps, _maps) != SNMP_ERR_NOERROR)
            snmp_log(LOG_ERR, "error registering for STORE_DATA callback "
                     "in netsnmp_certs_init\n");
    }
    _setup_containers();
}

void
netsnmp_certs_shutdown(void)
{
    DEBUGMSGT(("cert:util:shutdown","shutdown\n"));
    if (NULL != _certs) {
        CONTAINER_FREE_ALL(_certs, NULL);
        CONTAINER_FREE(_certs);
        _certs = NULL;
    }
    if (NULL != _keys) {
        CONTAINER_FREE_ALL(_keys, NULL);
        CONTAINER_FREE(_keys);
        _keys = NULL;
    }
}

void
netsnmp_certs_load(void)
{
    netsnmp_iterator  *itr;
    netsnmp_key        *key;
    netsnmp_cert       *cert;

    DEBUGMSGT(("cert:util:init","init\n"));

    if (NULL == _certs) {
        snmp_log(LOG_ERR, "cant load certs without container\n");
        return;
    }

    if (CONTAINER_SIZE(_certs) != 0) {
        DEBUGMSGT(("cert:util:init", "ignoring duplicate init\n"));
        return;
    }

    netsnmp_init_openssl();

    /** add certificate type mapping */
    se_add_pair_to_slist("cert_types", strdup("pem"), NS_CERT_TYPE_PEM);
    se_add_pair_to_slist("cert_types", strdup("crt"), NS_CERT_TYPE_DER);
    se_add_pair_to_slist("cert_types", strdup("cer"), NS_CERT_TYPE_DER);
    se_add_pair_to_slist("cert_types", strdup("cert"), NS_CERT_TYPE_DER);
    se_add_pair_to_slist("cert_types", strdup("der"), NS_CERT_TYPE_DER);
    se_add_pair_to_slist("cert_types", strdup("key"), NS_CERT_TYPE_KEY);
    se_add_pair_to_slist("cert_types", strdup("private"), NS_CERT_TYPE_KEY);

    /** hash algs */
    se_add_pair_to_slist("cert_hash_alg", strdup("sha1"), NS_HASH_SHA1);
    se_add_pair_to_slist("cert_hash_alg", strdup("md5"), NS_HASH_MD5);
    se_add_pair_to_slist("cert_hash_alg", strdup("sha224"), NS_HASH_SHA224);
    se_add_pair_to_slist("cert_hash_alg", strdup("sha256"), NS_HASH_SHA256);
    se_add_pair_to_slist("cert_hash_alg", strdup("sha384"), NS_HASH_SHA384);
    se_add_pair_to_slist("cert_hash_alg", strdup("sha512"), NS_HASH_SHA512);

    /** scan config dirs for certs */
    _cert_indexes_load();

    /** match up keys w/certs */
    itr = CONTAINER_ITERATOR(_keys);
    if (NULL == itr) {
        snmp_log(LOG_ERR, "could not get iterator for keys\n");
        netsnmp_certs_shutdown();
        return;
    }
    key = ITERATOR_FIRST(itr);
    for( ; key; key = ITERATOR_NEXT(itr))
        _find_partner(NULL, key);
    ITERATOR_RELEASE(itr);

    DEBUGIF("cert:dump") {
        itr = CONTAINER_ITERATOR(_certs);
        if (NULL == itr) {
            snmp_log(LOG_ERR, "could not get iterator for certs\n");
            netsnmp_certs_shutdown();
            return;
        }
        cert = ITERATOR_FIRST(itr);
        for( ; cert; cert = ITERATOR_NEXT(itr)) {
            netsnmp_cert_load_x509(cert);
        }
        ITERATOR_RELEASE(itr);
        DEBUGMSGT(("cert:dump",
                   "-------------------- Certificates -----------------\n"));
        netsnmp_cert_dump_all();
        DEBUGMSGT(("cert:dump",
                   "------------------------ End ----------------------\n"));
    }
}

/* #####################################################################
 *
 * cert container functions
 */

static netsnmp_container *
_get_cert_container(const char *use)
{
    netsnmp_container *c;

    c = netsnmp_container_find("certs:binary_array");
    if (NULL == c) {
        snmp_log(LOG_ERR, "could not create container for %s\n", use);
        return NULL;
    }
    c->container_name = strdup(use);
    c->free_item = (netsnmp_container_obj_func*)_cert_free;
    c->compare = (netsnmp_container_compare*)_cert_compare;

    return c;
}

static void
_setup_containers(void)
{
    netsnmp_container *additional_keys;

    _certs = _get_cert_container("netsnmp certificates");
    if (NULL == _certs)
        return;

    /** additional keys: common name */
    additional_keys = netsnmp_container_find("certs_cn:binary_array");
    if (NULL == additional_keys) {
        snmp_log(LOG_ERR, "could not create CN container for certificates\n");
        netsnmp_certs_shutdown();
        return;
    }
    additional_keys->container_name = strdup("certs_cn");
    additional_keys->free_item = NULL;
    additional_keys->compare = (netsnmp_container_compare*)_cert_cn_compare;
    netsnmp_container_add_index(_certs, additional_keys);

    /** additional keys: subject name */
    additional_keys = netsnmp_container_find("certs_sn:binary_array");
    if (NULL == additional_keys) {
        snmp_log(LOG_ERR, "could not create SN container for certificates\n");
        netsnmp_certs_shutdown();
        return;
    }
    additional_keys->container_name = strdup("certs_sn");
    additional_keys->free_item = NULL;
    additional_keys->compare = (netsnmp_container_compare*)_cert_sn_compare;
    additional_keys->ncompare = (netsnmp_container_compare*)_cert_sn_ncompare;
    netsnmp_container_add_index(_certs, additional_keys);

    /** additional keys: file name */
    additional_keys = netsnmp_container_find("certs_fn:binary_array");
    if (NULL == additional_keys) {
        snmp_log(LOG_ERR, "could not create FN container for certificates\n");
        netsnmp_certs_shutdown();
        return;
    }
    additional_keys->container_name = strdup("certs_fn");
    additional_keys->free_item = NULL;
    additional_keys->compare = (netsnmp_container_compare*)_cert_fn_compare;
    additional_keys->ncompare = (netsnmp_container_compare*)_cert_fn_ncompare;
    netsnmp_container_add_index(_certs, additional_keys);

    _keys = netsnmp_container_find("cert_keys:binary_array");
    if (NULL == _keys) {
        snmp_log(LOG_ERR, "could not create container for certificate keys\n");
        netsnmp_certs_shutdown();
        return;
    }
    _keys->container_name = strdup("netsnmp certificate keys");
    _keys->free_item = (netsnmp_container_obj_func*)_key_free;
    _keys->compare = (netsnmp_container_compare*)_cert_fn_compare;

    /*
     * container for cert to fingerprint mapping, with fingerprint key
     */
    _maps = netsnmp_cert_map_container_create(1);
}

netsnmp_container *
netsnmp_cert_map_container(void)
{
    return _maps;
}

static netsnmp_cert *
_new_cert(const char *dirname, const char *filename, int certType,
          int hashType, const char *fingerprint, const char *common_name,
          const char *subject)
{
    netsnmp_cert    *cert;

    if ((NULL == dirname) || (NULL == filename)) {
        snmp_log(LOG_ERR, "bad parameters to _new_cert\n");
        return NULL;
    }

    cert = SNMP_MALLOC_TYPEDEF(netsnmp_cert);
    if (NULL == cert) {
        snmp_log(LOG_ERR,"could not allocate memory for certificate at %s/%s\n",
                 dirname, filename);
        return NULL;
    }

    DEBUGMSGT(("9:cert:struct:new","new cert 0x%#lx for %s\n", (u_long)cert,
                  filename));

    cert->info.dir = strdup(dirname);
    cert->info.filename = strdup(filename);
    cert->info.allowed_uses = NS_CERT_REMOTE_PEER;
    cert->info.type = certType;
    if (fingerprint) {
        cert->hash_type = hashType;
        cert->fingerprint = strdup(fingerprint);
    }
    if (common_name)
        cert->common_name = strdup(common_name);
    if (subject)
        cert->subject = strdup(subject);

    return cert;
}

static netsnmp_key *
_new_key(const char *dirname, const char *filename)
{
    netsnmp_key    *key;
    struct stat     fstat;
    char            fn[SNMP_MAXPATH];

    if ((NULL == dirname) || (NULL == filename)) {
        snmp_log(LOG_ERR, "bad parameters to _new_key\n");
        return NULL;
    }

    /** check file permissions */
    snprintf(fn, sizeof(fn), "%s/%s", dirname, filename);
    if (stat(fn, &fstat) != 0) {
        snmp_log(LOG_ERR, "could  not stat %s\n", fn);
        return NULL;
    }

    if ((fstat.st_mode & S_IROTH) || (fstat.st_mode & S_IROTH)) {
        snmp_log(LOG_ERR,
                 "refusing to read world readable or writable key %s\n", fn);
        return NULL;
    }

    key = SNMP_MALLOC_TYPEDEF(netsnmp_key);
    if (NULL == key) {
        snmp_log(LOG_ERR, "could not allocate memory for key at %s/%s\n",
                 dirname, filename);
        return NULL;
    }

    DEBUGMSGT(("cert:key:struct:new","new key 0x%#lx for %s\n", (u_long)key,
               filename));

    key->info.type = NS_CERT_TYPE_KEY;
    key->info.dir = strdup(dirname);
    key->info.filename = strdup(filename);
    key->info.allowed_uses = NS_CERT_IDENTITY;

    return key;
}

void
netsnmp_cert_free(netsnmp_cert *cert)
{
    if (NULL == cert)
        return;

    DEBUGMSGT(("9:cert:struct:free","freeing cert 0x%#lx, %s (fp %s; CN %s)\n",
               (u_long)cert, cert->info.filename ? cert->info.filename : "UNK",
               cert->fingerprint ? cert->fingerprint : "UNK",
               cert->common_name ? cert->common_name : "UNK"));

    SNMP_FREE(cert->info.dir);
    SNMP_FREE(cert->info.filename);
    SNMP_FREE(cert->subject);
    SNMP_FREE(cert->issuer);
    SNMP_FREE(cert->fingerprint);
    SNMP_FREE(cert->common_name);
    if (cert->ocert)
        X509_free(cert->ocert);
    if (cert->key && cert->key->cert == cert)
        cert->key->cert = NULL;

    free(cert); /* SNMP_FREE not needed on parameters */
}

void
netsnmp_key_free(netsnmp_key *key)
{
    if (NULL == key)
        return;

    DEBUGMSGT(("cert:key:struct:free","freeing key 0x%#lx, %s\n",
               (u_long)key, key->info.filename ? key->info.filename : "UNK"));

    SNMP_FREE(key->info.dir);
    SNMP_FREE(key->info.filename);
    EVP_PKEY_free(key->okey);
    if (key->cert && key->cert->key == key)
        key->cert->key = NULL;

    free(key); /* SNMP_FREE not needed on parameters */
}

static void
_cert_free(netsnmp_cert *cert, void *context)
{
    netsnmp_cert_free(cert);
}

static void
_key_free(netsnmp_key *key, void *context)
{
    netsnmp_key_free(key);
}

static int
_cert_compare(netsnmp_cert *lhs, netsnmp_cert *rhs)
{
    netsnmp_assert((lhs != NULL) && (rhs != NULL));
    netsnmp_assert((lhs->fingerprint != NULL) &&
                   (rhs->fingerprint != NULL));

    /** ignore hash type? */
    return strcmp(lhs->fingerprint, rhs->fingerprint);
}

static int
_cert_path_compare(netsnmp_cert_common *lhs, netsnmp_cert_common *rhs)
{
    int rc;

    netsnmp_assert((lhs != NULL) && (rhs != NULL));
    
    /** dir name first */
    rc = strcmp(lhs->dir, rhs->dir);
    if (rc)
        return rc;

    /** filename */
    return strcmp(lhs->filename, rhs->filename);
}

static int
_cert_cn_compare(netsnmp_cert *lhs, netsnmp_cert *rhs)
{
    int rc;
    const char *lhcn, *rhcn;

    netsnmp_assert((lhs != NULL) && (rhs != NULL));

    if (NULL == lhs->common_name)
        lhcn = "";
    else
        lhcn = lhs->common_name;
    if (NULL == rhs->common_name)
        rhcn = "";
    else
        rhcn = rhs->common_name;

    rc = strcmp(lhcn, rhcn);
    if (rc)
        return rc;

    /** in case of equal common names, sub-sort by path */
    return _cert_path_compare((netsnmp_cert_common*)lhs,
                              (netsnmp_cert_common*)rhs);
}

static int
_cert_sn_compare(netsnmp_cert *lhs, netsnmp_cert *rhs)
{
    int rc;
    const char *lhsn, *rhsn;

    netsnmp_assert((lhs != NULL) && (rhs != NULL));

    if (NULL == lhs->subject)
        lhsn = "";
    else
        lhsn = lhs->subject;
    if (NULL == rhs->subject)
        rhsn = "";
    else
        rhsn = rhs->subject;

    rc = strcmp(lhsn, rhsn);
    if (rc)
        return rc;

    /** in case of equal common names, sub-sort by path */
    return _cert_path_compare((netsnmp_cert_common*)lhs,
                              (netsnmp_cert_common*)rhs);
}

static int
_cert_fn_compare(netsnmp_cert_common *lhs, netsnmp_cert_common *rhs)
{
    int rc;

    netsnmp_assert((lhs != NULL) && (rhs != NULL));

    rc = strcmp(lhs->filename, rhs->filename);
    if (rc)
        return rc;

    /** in case of equal common names, sub-sort by dir */
    return strcmp(lhs->dir, rhs->dir);
}

static int
_cert_fn_ncompare(netsnmp_cert_common *lhs, netsnmp_cert_common *rhs)
{
    netsnmp_assert((lhs != NULL) && (rhs != NULL));
    netsnmp_assert((lhs->filename != NULL) && (rhs->filename != NULL));

    return strncmp(lhs->filename, rhs->filename, strlen(rhs->filename));
}

static int
_cert_sn_ncompare(netsnmp_cert *lhs, netsnmp_cert *rhs)
{
    netsnmp_assert((lhs != NULL) && (rhs != NULL));
    netsnmp_assert((lhs->subject != NULL) && (rhs->subject != NULL));

    return strncmp(lhs->subject, rhs->subject, strlen(rhs->subject));
}

static int
_cert_ext_type(const char *ext)
{
    int rc = se_find_value_in_slist("cert_types", ext);
    if (SE_DNE == rc)
        return NS_CERT_TYPE_UNKNOWN;
    return rc;
}

static int
_type_from_filename(const char *filename)
{
    char     *pos;
    int       type;

    if (NULL == filename)
        return NS_CERT_TYPE_UNKNOWN;

    pos = strrchr(filename, '.');
    if (NULL == pos)
        return NS_CERT_TYPE_UNKNOWN;

    type = _cert_ext_type(++pos);
    return type;
}

/*
 * filter functions; return 1 to include file, 0 to exclude
 */
static int
_cert_cert_filter(const char *filename)
{
    int  len = strlen(filename);
    const char *pos;

    if (len < 5) /* shortest name: x.YYY */
        return 0;

    pos = strrchr(filename, '.');
    if (NULL == pos)
        return 0;

    if (_cert_ext_type(++pos) != NS_CERT_TYPE_UNKNOWN)
        return 1;

    return 0;
}

/* #####################################################################
 *
 * cert index functions
 *
 * This code mimics what the mib index code does. The persistent
 * directory will have a subdirectory named 'cert_indexes'. Inside
 * this directory will be some number of files with ascii numeric
 * names (0, 1, 2, etc). Each of these files will start with a line
 * with the text "DIR ", followed by a directory name. The rest of the
 * file will be certificate fields and the certificate file name, one
 * certificate per line. The numeric file name is the integer 'directory
 * index'.
 */

/**
 * _certindex_add
 *
 * add a directory name to the indexes
 */
static int
_certindex_add( const char *dirname, int i )
{
    int rc;
    char *dirname_copy = strdup(dirname);

    if ( i == -1 ) {
        int max = se_find_free_value_in_list(_certindexes);
        if (SE_DNE == max)
            i = 0;
        else
            i = max;
    }

    DEBUGMSGT(("cert:index:add","dir %s at index %d\n", dirname, i ));
    rc = se_add_pair_to_list(&_certindexes, dirname_copy, i);
    if (SE_OK != rc) {
        snmp_log(LOG_ERR, "adding certindex dirname failed; "
                 "%d (%s) not added\n", i, dirname);
        free(dirname_copy);
        return -1;
    }

    return i;
}

/**
 * _certindex_load
 *
 * read in the existing indexes
 */
static void
_certindexes_load( void )
{
    DIR *dir;
    struct dirent *file;
    FILE *fp;
    char filename[SNMP_MAXPATH], line[300];
    int  i;
    char *cp, *pos;

    /*
     * Open the CERT index directory, or create it (empty)
     */
    snprintf( filename, sizeof(filename), "%s/cert_indexes",
              get_persistent_directory());
    filename[sizeof(filename)-1] = 0;
    dir = opendir( filename );
    if ( dir == NULL ) {
        DEBUGMSGT(("cert:index:load",
                   "creating new cert_indexes directory\n"));
        mkdirhier( filename, NETSNMP_AGENT_DIRECTORY_MODE, 0);
        return;
    }

    /*
     * Create a list of which directory each file refers to
     */
    while ((file = readdir( dir ))) {
        if ( !isdigit(file->d_name[0]))
            continue;
        i = atoi( file->d_name );

        snprintf( filename, sizeof(filename), "%s/cert_indexes/%d",
              get_persistent_directory(), i );
        filename[sizeof(filename)-1] = 0;
        fp = fopen( filename, "r" );
        if ( !fp ) {
            DEBUGMSGT(("cert:index:load", "error opening index (%d)\n", i));
            fclose(fp);
            continue;
        }
        cp = fgets( line, sizeof(line), fp );
        if ( cp ) {
            line[strlen(line)-1] = 0;
            pos = strrchr(line, ' ');
            if (pos)
                *pos = '\0';
            DEBUGMSGT(("9:cert:index:load","adding (%d) %s\n", i, line));
            (void)_certindex_add( line+4, i );  /* Skip 'DIR ' */
        } else {
            DEBUGMSGT(("cert:index:load", "Empty index (%d)\n", i));
        }
        fclose( fp );
    }
    closedir( dir );
}

/**
 * _certindex_lookup
 *
 * find index for a directory
 */
static char *
_certindex_lookup( const char *dirname )
{
    int i;
    char filename[SNMP_MAXPATH];


    i = se_find_value_in_list(_certindexes, dirname);
    if (SE_DNE == i) {
        DEBUGMSGT(("9:cert:index:lookup","%s : (none)\n", dirname));
        return NULL;
    }

    snprintf(filename, sizeof(filename), "%s/cert_indexes/%d",
             get_persistent_directory(), i);
    filename[sizeof(filename)-1] = 0;
    DEBUGMSGT(("cert:index:lookup", "%s (%d) %s\n", dirname, i, filename ));
    return strdup(filename);
}

static FILE *
_certindex_new( const char *dirname )
{
    FILE *fp;
    char  filename[SNMP_MAXPATH], *cp;
    int   i;

    cp = _certindex_lookup( dirname );
    if (!cp) {
        i  = _certindex_add( dirname, -1 );
        if (-1 == i)
            return NULL; /* msg already logged */
        snprintf( filename, sizeof(filename), "%s/cert_indexes/%d",
                  get_persistent_directory(), i );
        filename[sizeof(filename)-1] = 0;
        cp = filename;
    }
    DEBUGMSGT(("9:cert:index:new", "%s (%s)\n", dirname, cp ));
    fp = fopen( cp, "w" );
    if (fp)
        fprintf( fp, "DIR %s %d\n", dirname, CERT_INDEX_FORMAT );
    else
        snmp_log(LOG_ERR, "error opening new index file %s\n", dirname);

    if (cp != filename)
        free(cp);

    return fp;
}

/* #####################################################################
 *
 * certificate utility functions
 *
 */
static X509 *
netsnmp_ocert_get(netsnmp_cert *cert)
{
    BIO            *certbio;
    X509           *ocert = NULL;
    EVP_PKEY       *okey = NULL;
    char            file[SNMP_MAXPATH];
    int             is_ca;

    if (NULL == cert)
        return NULL;

    if (cert->ocert)
        return cert->ocert;

    if (NS_CERT_TYPE_UNKNOWN == cert->info.type) {
        cert->info.type = _type_from_filename(cert->info.filename);
        if (NS_CERT_TYPE_UNKNOWN == cert->info.type) {
            snmp_log(LOG_ERR, "unknown certificate type %d for %s\n",
                     cert->info.type, cert->info.filename);
            return NULL;
        }
    }

    DEBUGMSGT(("9:cert:read", "Checking file %s\n", cert->info.filename));

    certbio = BIO_new(BIO_s_file());
    if (NULL == certbio) {
        snmp_log(LOG_ERR, "error creating BIO\n");
        return NULL;
    }

    snprintf(file, sizeof(file),"%s/%s", cert->info.dir, cert->info.filename);
    if (BIO_read_filename(certbio, file) <=0) {
        snmp_log(LOG_ERR, "error reading certificate %s into BIO\n", file);
        BIO_vfree(certbio);
        return NULL;
    }

    if (NS_CERT_TYPE_UNKNOWN == cert->info.type) {
        char *pos = strrchr(cert->info.filename, '.');
        if (NULL == pos)
            return NULL;
        cert->info.type = _cert_ext_type(++pos);
        netsnmp_assert(cert->info.type != NS_CERT_TYPE_UNKNOWN);
    }

    switch (cert->info.type) {

        case NS_CERT_TYPE_DER:
            ocert = d2i_X509_bio(certbio,NULL); /* DER/ASN1 */
            if (NULL != ocert)
                break;
            (void)BIO_reset(certbio);
            /** FALLTHROUGH: check for PEM if DER didn't work */

        case NS_CERT_TYPE_PEM:
            ocert = PEM_read_bio_X509_AUX(certbio, NULL, NULL, NULL);
            if (NULL == ocert)
                break;
            if (NS_CERT_TYPE_DER == cert->info.type) {
                DEBUGMSGT(("9:cert:read", "Changing type from DER to PEM\n"));
                cert->info.type = NS_CERT_TYPE_PEM;
            }
            /** check for private key too */
            if (NULL == cert->key) {
                (void)BIO_reset(certbio);
                okey =  PEM_read_bio_PrivateKey(certbio, NULL, NULL, NULL);
                if (NULL != okey) {
                    netsnmp_key  *key;
                    DEBUGMSGT(("cert:read:key", "found key with cert in %s\n",
                               cert->info.filename));
                    key = _new_key(cert->info.dir, cert->info.filename);
                    if (NULL != key) {
                        key->okey = okey;
                        if (-1 == CONTAINER_INSERT(_keys, key)) {
                            DEBUGMSGT(("cert:read:key:add",
                                       "error inserting key into container\n"));
                            netsnmp_key_free(key);
                            key = NULL;
                        }
                        else {
                            DEBUGMSGT(("cert:read:partner", "%s match found!\n",
                                       cert->info.filename));
                            key->cert = cert;
                            cert->key = key;
                            cert->info.allowed_uses |= NS_CERT_IDENTITY;
                        }
                    }
                } /* null return from read */
            } /* null key */
            break;
#ifdef CERT_PKCS12_SUPPORT_MAYBE_LATER
        case NS_CERT_TYPE_PKCS12:
            (void)BIO_reset(certbio);
            PKCS12 *p12 = d2i_PKCS12_bio(certbio, NULL);
            if ( (NULL != p12) && (PKCS12_verify_mac(p12, "", 0) ||
                                   PKCS12_verify_mac(p12, NULL, 0)))
                PKCS12_parse(p12, "", NULL, &cert, NULL);
            break;
#endif
        default:
            snmp_log(LOG_ERR, "unknown certificate type %d for %s\n",
                     cert->info.type, cert->info.filename);
    }

    BIO_vfree(certbio);

    if (NULL == ocert) {
        snmp_log(LOG_ERR, "error parsing certificate file %s\n",
                 cert->info.filename);
        return NULL;
    }

    cert->ocert = ocert;
    /*
     * X509_check_ca return codes:
     * 0 not a CA
     * 1 is a CA
     * 2 basicConstraints absent so "maybe" a CA
     * 3 basicConstraints absent but self signed V1.
     * 4 basicConstraints absent but keyUsage present and keyCertSign asserted.
     * 5 outdated Netscape Certificate Type CA extension.
     */
    is_ca = X509_check_ca(ocert);
    if (1 == is_ca)
        cert->info.allowed_uses |= NS_CERT_CA;

    if (NULL == cert->subject) {
        cert->subject = X509_NAME_oneline(X509_get_subject_name(ocert), NULL,
                                          0);
        DEBUGMSGT(("9:cert:add:subject", "subject name: %s\n", cert->subject));
    }

    if (NULL == cert->issuer) {
        cert->issuer = X509_NAME_oneline(X509_get_issuer_name(ocert), NULL, 0);
        if (strcmp(cert->subject, cert->issuer) == 0) {
            free(cert->issuer);
            cert->issuer = strdup("self-signed");
        }
        DEBUGMSGT(("9:cert:add:issuer", "CA issuer: %s\n", cert->issuer));
    }
    
    if (NULL == cert->fingerprint) {
        cert->hash_type = netsnmp_openssl_cert_get_hash_type(ocert);
        cert->fingerprint =
            netsnmp_openssl_cert_get_fingerprint(ocert, cert->hash_type);
    }
    
    if (NULL == cert->common_name) {
        cert->common_name =netsnmp_openssl_cert_get_commonName(ocert, NULL,
                                                               NULL);
        DEBUGMSGT(("9:cert:add:name","%s\n", cert->common_name));
    }

    return ocert;
}

EVP_PKEY *
netsnmp_okey_get(netsnmp_key  *key)
{
    BIO            *keybio;
    EVP_PKEY       *okey;
    char            file[SNMP_MAXPATH];

    if (NULL == key)
        return NULL;

    if (key->okey)
        return key->okey;

    snprintf(file, sizeof(file),"%s/%s", key->info.dir, key->info.filename);
    DEBUGMSGT(("cert:key:read", "Checking file %s\n", key->info.filename));

    keybio = BIO_new(BIO_s_file());
    if (NULL == keybio) {
        snmp_log(LOG_ERR, "error creating BIO\n");
        return NULL;
    }

    if (BIO_read_filename(keybio, file) <=0) {
        snmp_log(LOG_ERR, "error reading certificate %s into BIO\n",
                 key->info.filename);
        BIO_vfree(keybio);
        return NULL;
    }

    okey = PEM_read_bio_PrivateKey(keybio, NULL, NULL, NULL);
    if (NULL == okey)
        snmp_log(LOG_ERR, "error parsing certificate file %s\n",
                 key->info.filename);
    else
        key->okey = okey;

    BIO_vfree(keybio);

    return okey;
}

static netsnmp_cert *
_find_issuer(netsnmp_cert *cert)
{
    netsnmp_void_array *matching;
    netsnmp_cert       *candidate, *issuer = NULL;
    int                 i;

    if ((NULL == cert) || (NULL == cert->issuer))
        return NULL;

    /** find matching subject names */

    matching = _cert_find_subset_sn(cert->issuer);
    if (NULL == matching)
        return NULL;

    /** check each to see if it's the issuer */
    for ( i=0; (NULL == issuer) && (i < matching->size); ++i) {
        /** make sure we have ocert */
        candidate = (netsnmp_cert*)matching->array[i];
        if ((NULL == candidate->ocert) &&
            (netsnmp_ocert_get(candidate) == NULL))
            continue;

        /** compare **/
        if (netsnmp_openssl_cert_issued_by(candidate->ocert, cert->ocert))
            issuer = candidate;
    } /** candidate loop */

    free(matching->array);
    free(matching);

    return issuer;
}

int
netsnmp_cert_load_x509(netsnmp_cert *cert)
{
    int rc = 0;

    /** load ocert */
    if ((NULL == cert->ocert) && (netsnmp_ocert_get(cert) == NULL)) {
        DEBUGMSGT(("cert:load:err", "couldn't load cert for %s\n",
                   cert->info.filename));
        rc = -1;
    }

    /** load key */
    if ((NULL != cert->key) && (NULL == cert->key->okey) &&
        (netsnmp_okey_get(cert->key) == NULL)) {
        DEBUGMSGT(("cert:load:err", "couldn't load key for cert %s\n",
                   cert->info.filename));
        rc = -1;
    }

    /** make sure we have cert chain */
    for (; cert && cert->issuer; cert = cert->issuer_cert) {
        /** skip self signed */
        if (strcmp(cert->issuer, "self-signed") == 0) {
            netsnmp_assert(cert->issuer_cert == NULL);
            break;
        }
        /** get issuer cert */
        if (NULL == cert->issuer_cert) {
            cert->issuer_cert =  _find_issuer(cert);
            if (NULL == cert->issuer_cert) {
                DEBUGMSGT(("cert:load:err",
                           "couldn't load CA chain for cert %s\n",
                           cert->info.filename));
                rc = -1;
                break;
            }
        }
        /** get issuer ocert */
        if ((NULL == cert->issuer_cert->ocert) &&
            (netsnmp_ocert_get(cert->issuer_cert) == NULL)) {
            DEBUGMSGT(("cert:load:err", "couldn't load cert chain for %s\n",
                       cert->info.filename));
            rc = -1;
            break;
        }
    } /* cert CA for loop */

    return rc;
}

static void
_find_partner(netsnmp_cert *cert, netsnmp_key *key)
{
    netsnmp_void_array *matching;
    char                filename[NAME_MAX], *pos;

    if ((cert && key) || (!cert && ! key)) {
        DEBUGMSGT(("cert:partner", "bad parameters searching for partner\n"));
        return;
    }

    snprintf(filename, sizeof(filename), "%s", key->info.filename);
    pos = strrchr(filename, '.');
    if (NULL == pos)
        return;
    *pos = 0;

    if(key) {
        if (key->cert) {
            DEBUGMSGT(("cert:partner", "key already has partner\n"));
            return;
        }
        DEBUGMSGT(("9:cert:partner", "%s looking for partner near %s\n",
                   key->info.filename, key->info.dir));

        matching = _cert_find_subset_fn( filename, key->info.dir );
        if (!matching)
            return;
        if (1 == matching->size) {
            cert = (netsnmp_cert*)matching->array[0];
            if (NULL == cert->key) {
                DEBUGMSGT(("cert:partner", "%s match found!\n",
                           cert->info.filename));
                key->cert = cert;
                cert->key = key;
                cert->info.allowed_uses |= NS_CERT_IDENTITY;
            }
            else if (cert->key != key)
                snmp_log(LOG_ERR, "%s matching cert already has partner\n",
                         cert->info.filename);
        }
        else
            DEBUGMSGT(("cert:partner", "%s matches multiple certs\n",
                          key->info.filename));
    }
    else if(cert) {
        if (cert->key) {
            DEBUGMSGT(("cert:partner", "cert already has partner\n"));
            return;
        }
        DEBUGMSGT(("9:cert:partner", "%s looking for partner\n",
                   cert->info.filename));

        matching = _key_find_subset(filename);
        if (!matching)
            return;
        if (1 == matching->size) {
            key = (netsnmp_key*)matching->array[0];
            if (NULL == key->cert) {
                DEBUGMSGT(("cert:partner", "%s found!\n", cert->info.filename));
                key->cert = cert;
                cert->key = key;
            }
            else if (key->cert != cert)
                snmp_log(LOG_ERR, "%s matching key already has partner\n",
                         cert->info.filename);
        }
        else
            DEBUGMSGT(("cert:partner", "%s matches multiple keys\n",
                       cert->info.filename));
    }
    
    if (matching) {
        free(matching->array);
        free(matching);
    }
}

static int
_add_certfile(const char* dirname, const char* filename, FILE *index)
{
    X509         *ocert;
    EVP_PKEY     *okey;
    netsnmp_cert *cert = NULL;
    netsnmp_key  *key = NULL;
    char          certfile[SNMP_MAXPATH];
    int           type;

    if (((const void*)NULL == dirname) || (NULL == filename))
        return -1;

    type = _type_from_filename(filename);
    netsnmp_assert(type != NS_CERT_TYPE_UNKNOWN);

    snprintf(certfile, sizeof(certfile),"%s/%s", dirname, filename);

    DEBUGMSGT(("9:cert:file:add", "Checking file: %s (type %d)\n", filename,
               type));

    if (NS_CERT_TYPE_KEY == type) {
        key = _new_key(dirname, filename);
        if (NULL == key)
            return -1;
        okey = netsnmp_okey_get(key);
        if (NULL == okey) {
            netsnmp_key_free(key);
            return -1;
        }
        key->okey = okey;
        if (-1 == CONTAINER_INSERT(_keys, key)) {
            DEBUGMSGT(("cert:key:file:add:err",
                       "error inserting key into container\n"));
            netsnmp_key_free(key);
            key = NULL;
        }
    }
    else {
        cert = _new_cert(dirname, filename, type, -1, NULL, NULL, NULL);
        if (NULL == cert)
            return -1;
        ocert = netsnmp_ocert_get(cert);
        if (NULL == ocert) {
            netsnmp_cert_free(cert);
            return -1;
        }
        cert->ocert = ocert;
        if (-1 == CONTAINER_INSERT(_certs, cert)) {
            DEBUGMSGT(("cert:file:add:err",
                       "error inserting cert into container\n"));
            netsnmp_cert_free(cert);
            cert = NULL;
        }
    }
    if ((NULL == cert) && (NULL == key)) {
        DEBUGMSGT(("cert:file:add:failure", "for %s\n", certfile));
        return -1;
    }

    if (index) {
        /** filename = NAME_MAX = 255 */
        /** fingerprint = 60 */
        /** common name / CN  = 64 */
        if (cert)
            fprintf(index, "c:%s %d %d %s '%s' '%s'\n", filename,
                    cert->info.type, cert->hash_type, cert->fingerprint,
                    cert->common_name, cert->subject);
        else if (key)
            fprintf(index, "k:%s\n", filename);
    }

    return 0;
}

static int
_cert_read_index(const char *dirname, struct stat *dirstat)
{
    FILE           *index;
    char           *idxname, *pos;
    struct stat     idx_stat;
    char            tmpstr[SNMP_MAXPATH + 5], filename[NAME_MAX];
    char            fingerprint[60+1], common_name[64+1], type_str[15];
    char            subject[SNMP_MAXBUF_SMALL], hash_str[15];
    int             count = 0, type, hash, version;
    netsnmp_cert    *cert;
    netsnmp_key     *key;
    netsnmp_container *newer, *found;

    netsnmp_assert(NULL != dirname);

    idxname = _certindex_lookup( dirname );
    if (NULL == idxname) {
        DEBUGMSGT(("cert:index:parse", "no index for cert directory\n"));
        return -1;
    }

    /*
     * see if directory has been modified more recently than the index
     */
    if (stat(idxname, &idx_stat) != 0) {
        DEBUGMSGT(("cert:index:parse", "error getting index file stats\n"));
        SNMP_FREE(idxname);
        return -1;
    }

#if (defined(WIN32) || defined(cygwin))
    /* For Win32 platforms, the directory does not maintain a last modification
     * date that we can compare with the modification date of the .index file.
     */
#else
    if (dirstat->st_mtime >= idx_stat.st_mtime) {
        DEBUGMSGT(("cert:index:parse", "Index outdated; dir modified\n"));
        SNMP_FREE(idxname);
        return -1;
    }
#endif

    /*
     * dir mtime doesn't change when files are touched, so we need to check
     * each file against the index in case a file has been modified.
     */
    newer =
        netsnmp_directory_container_read_some(NULL, dirname,
                                              (netsnmp_directory_filter*)
                                              _time_filter,(void*)&idx_stat,
                                              NETSNMP_DIR_NSFILE |
                                              NETSNMP_DIR_NSFILE_STATS);
    if (newer) {
        DEBUGMSGT(("cert:index:parse", "Index outdated; files modified\n"));
        CONTAINER_FREE_ALL(newer, NULL);
        CONTAINER_FREE(newer);
        SNMP_FREE(idxname);
        return -1;
    }

    DEBUGMSGT(("cert:index:parse", "The index for %s looks good\n", dirname));

    index = fopen(idxname, "r");
    if (NULL == index) {
        snmp_log(LOG_ERR, "cert:index:parse can't open index for %s\n",
            dirname);
        SNMP_FREE(idxname);
        return -1;
    }

    found = _get_cert_container(idxname);

    /*
     * check index format version
     */
    fgets(tmpstr, sizeof(tmpstr), index);
    pos = strrchr(tmpstr, ' ');
    if (pos) {
        ++pos;
        version = atoi(pos);
    }
    if ((NULL == pos) || (version != CERT_INDEX_FORMAT)) {
        DEBUGMSGT(("cert:index:add", "missing or wrong index format!\n"));
        fclose(index);
        SNMP_FREE(idxname);
        return -1;
    }
    while (1) {
        if (NULL == fgets(tmpstr, sizeof(tmpstr), index))
            break;

        if ('c' == tmpstr[0]) {
            pos = &tmpstr[2];
            if ((NULL == (pos=copy_nword(pos, filename, sizeof(filename)))) ||
                (NULL == (pos=copy_nword(pos, type_str, sizeof(type_str)))) ||
                (NULL == (pos=copy_nword(pos, hash_str, sizeof(hash_str)))) ||
                (NULL == (pos=copy_nword(pos, fingerprint,
                                         sizeof(fingerprint)))) ||
                (NULL == (pos=copy_nword(pos, common_name,
                                           sizeof(common_name)))) ||
                (NULL != copy_nword(pos, subject, sizeof(subject)))) {
                snmp_log(LOG_ERR, "_cert_read_index: error parsing line: %s\n",
                         tmpstr);
                count = -1;
                break;
            }
            type = atoi(type_str);
            hash = atoi(hash_str);
            cert = (void*)_new_cert(dirname, filename, type, hash, fingerprint,
                                    common_name, subject);
            if (cert && 0 == CONTAINER_INSERT(found, cert))
                ++count;
            else {
                DEBUGMSGT(("cert:index:add",
                           "error inserting cert into container\n"));
                netsnmp_cert_free(cert);
                cert = NULL;
            }
        }
        else if ('k' == tmpstr[0]) {
            if (NULL != copy_nword(&tmpstr[2], filename, sizeof(filename))) {
                snmp_log(LOG_ERR, "_cert_read_index: error parsing line %s\n",
                    tmpstr);
                continue;
            }
            key = _new_key(dirname, filename);
            if (key && 0 == CONTAINER_INSERT(_keys, key))
                ++count;
            else {
                DEBUGMSGT(("cert:index:add:key",
                           "error inserting key into container\n"));
                netsnmp_key_free(key);
            }
        }
        else {
            snmp_log(LOG_ERR, "unknown line in cert index for %s\n", dirname);
            continue;
        }
    } /* while */
    fclose(index);
    SNMP_FREE(idxname);

    if (count > 0) {
        netsnmp_iterator  *itr = CONTAINER_ITERATOR(found);
        if (NULL == itr) {
            snmp_log(LOG_ERR, "could not get iterator for found certs\n");
            count = -1;
        }
        else {
            cert = ITERATOR_FIRST(itr);
            for( ; cert; cert = ITERATOR_NEXT(itr))
                CONTAINER_INSERT(_certs, cert);
            ITERATOR_RELEASE(itr);
            DEBUGMSGT(("cert:index:parse","added %d certs from index\n",
                       count));
        }
    }
    if (count < 0)
        CONTAINER_FREE_ALL(found, NULL);
    CONTAINER_FREE(found);

    return count;
}

static int
_add_certdir(const char *dirname)
{
    FILE           *index;
    char           *file;
    int             count = 0;
    netsnmp_container *cert_container;
    netsnmp_iterator  *it;
    struct stat     statbuf;

    netsnmp_assert(NULL != dirname);

    DEBUGMSGT(("9:cert:dir:add", " config dir: %s\n", dirname ));

    if (stat(dirname, &statbuf) != 0) {
        DEBUGMSGT(("9:cert:dir:add", " dir not present: %s\n",
                   dirname ));
        return -1;
    }
#ifdef S_ISDIR
    if (!S_ISDIR(statbuf.st_mode)) {
        DEBUGMSGT(("9:cert:dir:add", " not a dir: %s\n", dirname ));
        return -1;
    }
#endif

    DEBUGMSGT(("cert:index:dir", "Scanning directory %s\n", dirname));

    /*
     * look for existing index
     */
    count = _cert_read_index(dirname, &statbuf);
    if (count >= 0)
        return count;

    index = _certindex_new( dirname );
    if (NULL == index) {
        DEBUGMSGT(("9:cert:index:dir",
                    "error opening index for cert directory\n"));
        NETSNMP_LOGONCE((LOG_ERR, "could not open certificate index file\n"));
    }

    /*
     * index was missing, out of date or bad. rescan directory.
     */
    cert_container =
        netsnmp_directory_container_read_some(NULL, dirname,
                                              (netsnmp_directory_filter*)
                                              &_cert_cert_filter, NULL,
                                              NETSNMP_DIR_RELATIVE_PATH |
                                              NETSNMP_DIR_EMPTY_OK );
    if (NULL == cert_container) {
        DEBUGMSGT(("cert:index:dir",
                    "error creating container for cert files\n"));
        goto err_index;
    }

    /*
     * iterate through the found files and add them to index
     */
    it = CONTAINER_ITERATOR(cert_container);
    if (NULL == it) {
        DEBUGMSGT(("cert:index:dir",
                    "error creating iterator for cert files\n"));
        goto err_container;
    }

    for (file = ITERATOR_FIRST(it); file; file = ITERATOR_NEXT(it)) {
        DEBUGMSGT(("cert:index:dir", "adding %s to index\n", file));
        if ( 0 == _add_certfile( dirname, file, index ))
            count++;
        else
            DEBUGMSGT(("cert:index:dir", "error adding %s to index\n",
                        file));
    }

    /*
     * clean up and return
     */
    ITERATOR_RELEASE(it);

  err_container:
    netsnmp_directory_container_free(cert_container);

  err_index:
    if (index)
        fclose(index);

    return count;
}

static void
_cert_indexes_load(void)
{
    const char     *confpath;
    char           *confpath_copy, *dir, *st = NULL;
    char            certdir[SNMP_MAXPATH];
    const char     *subdirs[] = { NULL, "ca-certs", "certs", "private", NULL };
    int             i = 0;

    /*
     * load indexes from persistent dir
     */
    _certindexes_load();

    /*
     * duplicate path building from read_config_files_of_type() in
     * read_config.c. That is, use SNMPCONFPATH environment variable if
     * it is defined, otherwise use configuration directory.
     */
    confpath = netsnmp_getenv("SNMPCONFPATH");
    if (NULL == confpath)
        confpath = get_configuration_directory();

    subdirs[0] = netsnmp_ds_get_string(NETSNMP_DS_LIBRARY_ID,
                                       NETSNMP_DS_LIB_CERT_EXTRA_SUBDIR);
    confpath_copy = strdup(confpath);
    for ( dir = strtok_r(confpath_copy, ENV_SEPARATOR, &st);
          dir; dir = strtok_r(NULL, ENV_SEPARATOR, &st)) {

        i = (NULL == subdirs[0]) ? 1 : 0;
        for ( ; subdirs[i] ; ++i ) {
            /** check tls subdir */
            snprintf(certdir, sizeof(certdir), "%s/tls/%s", dir, subdirs[i]);
            _add_certdir(certdir);
        } /* for subdirs */
    } /* for conf path dirs */
    SNMP_FREE(confpath_copy);
}

static void
_cert_print(netsnmp_cert *c, void *context)
{
    if (NULL == c)
        return;

    DEBUGMSGT(("cert:dump", "cert %s in %s\n", c->info.filename, c->info.dir));
    DEBUGMSGT(("cert:dump", "   type %d flags 0x%x (%s)\n",
             c->info.type, c->info.allowed_uses,
              _mode_str(c->info.allowed_uses)));
    if (NS_CERT_TYPE_KEY == c->info.type) {
    }
    else {

        if(c->subject) {
            if (c->info.allowed_uses & NS_CERT_CA)
                DEBUGMSGT(("cert:dump", "   CA: %s\n", c->subject));
            else
                DEBUGMSGT(("cert:dump", "   subject: %s\n", c->subject));
        }
        if(c->issuer)
            DEBUGMSGT(("cert:dump", "   issuer: %s\n", c->issuer));
        if(c->fingerprint)
            DEBUGMSGT(("cert:dump", "   fingerprint: %s(%d):%s\n",
                       se_find_label_in_slist("cert_hash_alg", c->hash_type),
                       c->hash_type, c->fingerprint));
    }

    DEBUGIF("9:cert:dump")
        netsnmp_openssl_cert_dump_names(c->ocert);

    netsnmp_openssl_cert_dump_extensions(c->ocert);
    
}

static void
_key_print(netsnmp_key *k, void *context)
{
    if (NULL == k)
        return;

    DEBUGMSGT(("cert:dump", "key found in %s %s\n", k->info.dir,
               k->info.filename));
    DEBUGMSGT(("cert:dump", "   type %d flags 0x%x (%s)\n", k->info.type,
              k->info.allowed_uses, _mode_str(k->info.allowed_uses)));
}

void
netsnmp_cert_dump_all(void)
{
    CONTAINER_FOR_EACH(_certs, (netsnmp_container_obj_func*)_cert_print, NULL);
    CONTAINER_FOR_EACH(_keys, (netsnmp_container_obj_func*)_key_print, NULL);
}

#ifdef CERT_MAIN
/*
 * export BLD=~/net-snmp/build/ SRC=~/net-snmp/src 
 * cc -DCERT_MAIN `$BLD/net-snmp-config --cflags` `$BLD/net-snmp-config --build-includes $BLD/`  $SRC/snmplib/cert_util.c   -o cert_util `$BLD/net-snmp-config --build-lib-dirs $BLD` `$BLD/net-snmp-config --libs` -lcrypto -lssl
 *
 */
int
main(int argc, char** argv)
{
    int          ch;
    extern char *optarg;

    while ((ch = getopt(argc, argv, "D:fHLMx:")) != EOF)
        switch(ch) {
            case 'D':
                debug_register_tokens(optarg);
                snmp_set_do_debugging(1);
                break;
            default:
                fprintf(stderr,"unknown option %c\n", ch);
        }

    init_snmp("dtlsapp");

    netsnmp_cert_dump_all();

    return 0;
}

#endif /* CERT_MAIN */

static netsnmp_cert *_cert_find_fp(const char *fingerprint);

void
netsnmp_fp_lowercase_and_strip_colon(char *fp)
{
    char *pos, *dest=NULL;
    
    if(!fp)
        return;

    /** skip to first : */
    for (pos = fp; *pos; ++pos ) {
        if (':' == *pos) {
            dest = pos;
            break;
        }
        else
            *pos = isalpha(*pos) ? tolower(*pos) : *pos;
    }
    if (!*pos)
        return;

    /** copy, skipping any ':' */
    for (++pos; *pos; ++pos) {
        if (':' == *pos)
            continue;
        *dest++ = isalpha(*pos) ? tolower(*pos) : *pos;
    }
    *dest = *pos; /* nul termination */
}

netsnmp_cert *
netsnmp_cert_find(int what, int where, void *hint)
{
    netsnmp_cert *result = NULL;
    int           tmp;
    char         *fp;

    DEBUGMSGT(("cert:find:params", "looking for %s(%d) in %s(0x%x), hint %lu\n",
               _mode_str(what), what, _where_str(where), where, (u_long)hint));

    if (NS_CERTKEY_DEFAULT == where) {
            
        switch (what) {
            case NS_CERT_IDENTITY: /* want my ID */
                tmp = (ptrdiff_t)hint;
                fp =
                    netsnmp_ds_get_string(NETSNMP_DS_LIBRARY_ID,
                                          tmp ? NETSNMP_DS_LIB_X509_SERVER_PUB :
                                          NETSNMP_DS_LIB_X509_CLIENT_PUB );
                break;
            case NS_CERT_REMOTE_PEER:
                fp = netsnmp_ds_get_string(NETSNMP_DS_LIBRARY_ID,
                                           NETSNMP_DS_LIB_X509_SERVER_PUB);
                break;
            default:
                DEBUGMSGT(("cert:find:err", "unhandled type %d for %s(%d)\n",
                           what, _where_str(where), where));
                return NULL;
        }
        result = _cert_find_fp(fp);

    } /* where = ds store */
    else if (NS_CERTKEY_FINGERPRINT == where) {
        result = _cert_find_fp((char *)hint);
    }
    else if (NS_CERTKEY_TARGET_ADDR == where) {
        
        /** hint == target mib data */
        switch (what) {
            case NS_CERT_IDENTITY:
            case NS_CERT_REMOTE_PEER:
            default:
                DEBUGMSGT(("cert:find:err", "unhandled type %d for %d\n", what,
                           where));
                return NULL;
        }
    } /* where = target mib */
    else { /* unknown location */
        
        DEBUGMSGT(("cert:find:err", "unhandled location %d for %d\n", where,
                   what));
        return NULL;
    }
    
    if (NULL == result)
        return NULL;

    /** make sure result found can be used for specified type */
    if (!(result->info.allowed_uses & what)) {
        DEBUGMSGT(("cert:find:err",
                   "cert %s / %s not allowed for %s(%d) (uses=%s (%d))\n",
                   result->info.filename, result->fingerprint, _mode_str(what),
                   what , _mode_str(result->info.allowed_uses),
                   result->info.allowed_uses));
        return NULL;
    }
    
    /** make sure we have the cert data */
    if (netsnmp_cert_load_x509(result) < 0)
        return NULL;
            
    return result;
}

int
netsnmp_cert_validate(int who, int how, X509 *cert)
{
    return -1;
}

int
netsnmp_cert_check_vb_fingerprint(const netsnmp_variable_list *var)
{
    if (!var)
        return SNMP_ERR_GENERR;

    if (0 == var->val_len) /* empty allowed in some cases */
        return SNMP_ERR_NOERROR;

    if (! (0x01 & var->val_len)) { /* odd len */
        DEBUGMSGT(("cert:varbind:fingerprint",
                   "expecting odd length for fingerprint\n"));
        return SNMP_ERR_WRONGLENGTH;
    }

    if (var->val.string[0] > NS_HASH_MAX) {
        DEBUGMSGT(("cert:varbind:fingerprint", "hashtype %d > max %d\n",
                   var->val.string[0], NS_HASH_MAX));
        return SNMP_ERR_WRONGVALUE;
    }

    return SNMP_ERR_NOERROR;
}

/* ***************************************************************************
 *
 * mode text functions
 *
 */
static const char *_mode_str(u_char mode)
{
    return _modes[mode];
}

static const char *_where_str(u_int what)
{
    switch (what) {
        case NS_CERTKEY_DEFAULT: return "DEFAULT";
        case NS_CERTKEY_FILE: return "FILE";
        case NS_CERTKEY_FINGERPRINT: return "FINGERPRINT";
        case NS_CERTKEY_CA: return "CA";
        case NS_CERTKEY_SAN_RFC822: return "SAN_RFC822";
        case NS_CERTKEY_SAN_DNS: return "SAN_DNS";
        case NS_CERTKEY_SAN_IPADDR: return "SAN_IPADDR";
        case NS_CERTKEY_COMMON_NAME: return "COMMON_NAME";
        case NS_CERTKEY_TARGET_PARAM: return "TARGET_PARAM";
        case NS_CERTKEY_TARGET_ADDR: return "TARGET_ADDR";
    }

    return "UNKNOWN";
}

/* ***************************************************************************
 *
 * find functions
 *
 */
static netsnmp_cert *
_cert_find_fp(const char *fingerprint)
{
    netsnmp_cert cert, *result = NULL;
    char         fp[EVP_MAX_MD_SIZE];

    if (NULL == fingerprint)
        return NULL;

    strncpy(fp, fingerprint, sizeof(fp));
    netsnmp_fp_lowercase_and_strip_colon(fp);

    /** clear search key */
    memset(&cert, 0x00, sizeof(cert));

    cert.fingerprint = fp;

    result = CONTAINER_FIND(_certs,&cert);
    return result;
}

/*
 * reduce subset by eliminating any filenames that are longer than
 * the specified file name. e.g. 'snmp' would match 'snmp.key' and
 * 'snmpd.key'. We only want 'snmp.X', where X is a valid extension.
 */
static void
_reduce_subset(netsnmp_void_array *matching, const char *filename)
{
    netsnmp_cert_common *cc;
    int i = 0, j, newsize, pos;

    if ((NULL == matching) || (NULL == filename))
        return;

    pos = strlen(filename);
    newsize = matching->size;

    for( ; i < matching->size; ) {
        /*
         * if we've shifted matches down we'll hit a NULL entry before
         * we hit the end of the array.
         */
        if (NULL == matching->array[i])
            break;
        /*
         * skip over valid matches. Note that we do not want to use
         * _type_from_filename.
         */
        cc = (netsnmp_cert_common*)matching->array[i];
        if (('.' == cc->filename[pos]) &&
            (NS_CERT_TYPE_UNKNOWN != _cert_ext_type(&cc->filename[pos+1]))) {
            ++i;
            continue;
        }
        /*
         * shrink array by shifting everything down a spot. Might not be
         * the most efficient soloution, but this is just happening at
         * startup and hopefully most certs won't have common prefixes.
         */
        --newsize;
        for ( j=i; j < newsize; ++j )
            matching->array[j] = matching->array[j+1];
        matching->array[j] = NULL;
        /** no ++i; just shifted down, need to look at same position again */
    }
    /*
     * if we shifted, set the new size
     */
    if (newsize != matching->size) {
        DEBUGMSGT(("9:cert:subset:reduce", "shrank from %" NETSNMP_PRIz "d to %d\n",
                   matching->size, newsize));
        matching->size = newsize;
    }
}

/*
 * reduce subset by eliminating any filenames that are not under the
 * specified directory path.
 */
static void
_reduce_subset_dir(netsnmp_void_array *matching, const char *directory)
{
    netsnmp_cert_common *cc;
    int                  i = 0, j, newsize, dir_len;
    char                 dir[SNMP_MAXPATH], *pos;

    if ((NULL == matching) || (NULL == directory))
        return;

    newsize = matching->size;

    /*
     * dir struct should be something like
     *          /usr/share/snmp/tls/certs
     *          /usr/share/snmp/tls/private
     *
     * so we want to backup up on directory for compares..
     */
    strncpy(dir,directory,sizeof(dir));
    pos = strrchr(dir, '/');
    if (NULL == pos) {
        DEBUGMSGTL(("cert:subset:dir", "no '/' in directory %s\n", directory));
        return;
    }
    *pos = '\0';
    dir_len = strlen(dir);

    for( ; i < matching->size; ) {
        /*
         * if we've shifted matches down we'll hit a NULL entry before
         * we hit the end of the array.
         */
        if (NULL == matching->array[i])
            break;
        /*
         * skip over valid matches. 
         */
        cc = (netsnmp_cert_common*)matching->array[i];
        if (strncmp(dir, cc->dir, dir_len) == 0) {
            ++i;
            continue;
        }
        /*
         * shrink array by shifting everything down a spot. Might not be
         * the most efficient soloution, but this is just happening at
         * startup and hopefully most certs won't have common prefixes.
         */
        --newsize;
        for ( j=i; j < newsize; ++j )
            matching->array[j] = matching->array[j+1];
        matching->array[j] = NULL;
        /** no ++i; just shifted down, need to look at same position again */
    }
    /*
     * if we shifted, set the new size
     */
    if (newsize != matching->size) {
        DEBUGMSGT(("9:cert:subset:dir", "shrank from %" NETSNMP_PRIz "d to %d\n",
                   matching->size, newsize));
        matching->size = newsize;
    }
}

static netsnmp_void_array *
_cert_find_subset_common(const char *filename, netsnmp_container *container)
{
    netsnmp_cert_common   search;
    netsnmp_void_array   *matching;

    netsnmp_assert(filename && container);

    memset(&search, 0x00, sizeof(search));    /* clear search key */

    search.filename = NETSNMP_REMOVE_CONST(char*,filename);

    matching = CONTAINER_GET_SUBSET(container, &search);
    DEBUGMSGT(("9:cert:subset:found", "%" NETSNMP_PRIz "d matches\n", matching ?
               matching->size : 0));
    if (matching && matching->size > 1)
        _reduce_subset(matching, filename);
    return matching;
}

static netsnmp_void_array *
_cert_find_subset_fn(const char *filename, const char *directory)
{
    netsnmp_container    *fn_container;
    netsnmp_void_array   *matching;

    /** find subcontainer with filename as key */
    fn_container = SUBCONTAINER_FIND(_certs, "certs_fn");
    netsnmp_assert(fn_container);

    matching = _cert_find_subset_common(filename, fn_container);
    if (matching && (matching->size > 1) && directory)
        _reduce_subset_dir(matching, directory);
    return matching;
}

static netsnmp_void_array *
_cert_find_subset_sn(const char *subject)
{
    netsnmp_cert          search;
    netsnmp_void_array   *matching;
    netsnmp_container    *sn_container;

    /** find subcontainer with subject as key */
    sn_container = SUBCONTAINER_FIND(_certs, "certs_sn");
    netsnmp_assert(sn_container);

    memset(&search, 0x00, sizeof(search));    /* clear search key */

    search.subject = NETSNMP_REMOVE_CONST(char*,subject);

    matching = CONTAINER_GET_SUBSET(sn_container, &search);
    DEBUGMSGT(("9:cert:subset:found", "%" NETSNMP_PRIz "d matches\n", matching ?
               matching->size : 0));
    return matching;
}

static netsnmp_void_array *
_key_find_subset(const char *filename)
{
    return _cert_find_subset_common(filename, _keys);
}

#if 0  /* not used yet */
static netsnmp_key *
_key_find_fn(const char *filename)
{
    netsnmp_key key, *result = NULL;

    netsnmp_assert(NULL != filename);

    memset(&key, 0x00, sizeof(key));    /* clear search key */
    key.info.filename = NETSNMP_REMOVE_CONST(char*,filename);
    result = CONTAINER_FIND(_keys,&key);
    return result;
}
#endif

static int
_time_filter(netsnmp_file *f, struct stat *idx)
{
    /** include if mtime or ctime newer than index mtime */
    if (f && idx && f->stats &&
        ((f->stats->st_mtime >= idx->st_mtime) ||
         (f->stats->st_ctime >= idx->st_mtime)))
        return NETSNMP_DIR_INCLUDE;

    return NETSNMP_DIR_EXCLUDE;
}

/* ***************************************************************************
 * ***************************************************************************
 *
 *
 * cert map functions
 *
 *
 * ***************************************************************************
 * ***************************************************************************/
static void _map_free(netsnmp_cert_map* entry, void *ctx);

netsnmp_cert_map *
netsnmp_cert_map_alloc(char *fingerprint, X509 *ocert)
{
    netsnmp_cert_map *cert_map = SNMP_MALLOC_TYPEDEF(netsnmp_cert_map);
    if (NULL == cert_map) {
        snmp_log(LOG_ERR, "could not allocate netsnmp_cert_map\n");
        return NULL;
    }
    
    if (fingerprint) {
        /** MIB limits to 255 bytes; 2x since we've got ascii */
        if (strlen(fingerprint) > (SNMPADMINLENGTH * 2)) {
            snmp_log(LOG_ERR, "fingerprint %s exceeds max length %d\n",
                     fingerprint, (SNMPADMINLENGTH * 2));
            free(cert_map);
            return NULL;
        }
        cert_map->fingerprint = strdup(fingerprint);
        cert_map->hashType = netsnmp_openssl_cert_get_hash_type(ocert);
    }
    if (ocert)
        cert_map->ocert = ocert;

    return cert_map;
}

void
netsnmp_cert_map_free(netsnmp_cert_map *cert_map)
{
    if (NULL == cert_map)
        return;

    SNMP_FREE(cert_map->fingerprint);
    SNMP_FREE(cert_map->data);
    /** x509 cert isn't ours */
    free(cert_map); /* SNMP_FREE wasted on param */
}

static void
_map_free(netsnmp_cert_map *map, void *context)
{
    netsnmp_cert_map_free(map);
}

static int
_map_compare(netsnmp_cert_map *lhs, netsnmp_cert_map *rhs)
{
    netsnmp_assert((lhs != NULL) && (rhs != NULL));

    if (lhs->priority < rhs->priority)
        return -1;
    else if (lhs->priority > rhs->priority)
        return 1;

    return strcmp(lhs->fingerprint, rhs->fingerprint);
}

static int
_map_fp_compare(netsnmp_cert_map *lhs, netsnmp_cert_map *rhs)
{
    int rc;
    netsnmp_assert((lhs != NULL) && (rhs != NULL));

    if ((rc = strcmp(lhs->fingerprint, rhs->fingerprint)) != 0)
        return rc;

    if (lhs->priority < rhs->priority)
        return -1;
    else if (lhs->priority > rhs->priority)
        return 1;

    return 0;
}

static int
_map_fp_ncompare(netsnmp_cert_map *lhs, netsnmp_cert_map *rhs)
{
    netsnmp_assert((lhs != NULL) && (rhs != NULL));

    return strncmp(lhs->fingerprint, rhs->fingerprint,
                   strlen(rhs->fingerprint));
}

netsnmp_container *
netsnmp_cert_map_container_create(int with_fp)
{
    netsnmp_container *chain_map =
        netsnmp_container_find("stack:binary_array");
    netsnmp_container *fp;

    if (NULL == chain_map) {
        snmp_log(LOG_ERR, "could not allocate container for cert_map\n");
        return NULL;
    }

    chain_map->free_item = (netsnmp_container_obj_func*)_map_free;
    chain_map->compare = (netsnmp_container_compare*)_map_compare;

    if (!with_fp)
        return chain_map;

    /*
     * add a secondary index to the table container
     */
    fp = netsnmp_container_find("cert2sn_fp:binary_array");
    if (NULL == fp) {
        snmp_log(LOG_ERR,
                 "error creating sub-containter for tlstmCertToTSNTable\n");
        CONTAINER_FREE(chain_map);
        return NULL;
    }
    fp->container_name = strdup("cert2sn_fp");
    fp->compare = (netsnmp_container_compare*)_map_fp_compare;
    fp->ncompare = (netsnmp_container_compare*)_map_fp_ncompare;
    netsnmp_container_add_index(chain_map, fp);

    return chain_map;
}


static int
_parse_ht_str(const char *str)
{
    int rc = se_find_value_in_slist("cert_hash_alg", str);
    if (SE_DNE == rc)
        return NS_HASH_NONE;
    return rc;
}

void
netsnmp_cert_map_container_free(netsnmp_container *c)
{
    if (NULL == c)
        return;

    CONTAINER_FREE_ALL(c, NULL);
    CONTAINER_FREE(c);
}

/*
  certSecName PRIORITY [--shaNN|md5] FINGERPRINT <--sn SECNAME | --rfc822 | --dns | --ip | --cn | --any>

  certSecName  100  ff:..11 --sn Wes
  certSecName  200  ee:..:22 --sn JohnDoe
  certSecName  300  ee:..:22 --rfc822
*/
#define CONFIG_TOKEN "certSecName"
static void
_parse_map(const char *token, char *line)
{
    netsnmp_cert_map *map;
    char             *tmp, buf[SNMP_MAXBUF_SMALL];
    size_t            len;

    /** need somewhere to save rows */
    if (NULL == _maps) {
        NETSNMP_LOGONCE((LOG_ERR, "no container for certificate mappings\n"));
        return;
    }

    DEBUGMSGT(("cert:util:config", "parsing %s\n", line));

    len = sizeof(buf);
    tmp = buf;
    line = read_config_read_octet_string(line, (u_char **)&tmp, &len);
    if (!isdigit(tmp[0])) {
        netsnmp_config_error(CONFIG_TOKEN ": could not parse priority");
        return;
    }
    map = netsnmp_cert_map_alloc(NULL, NULL);
    if (NULL == map) {
        netsnmp_config_error(CONFIG_TOKEN
                             ": could not allocate cert map struct");
        return;
    }
    map->flags |= NSCM_FROM_CONFIG;
    map->priority = atoi(buf);

    len = sizeof(buf);
    tmp = buf;
    line = read_config_read_octet_string(line, (u_char **)&tmp, &len);
    if ((buf[0] == '-') && (buf[1] == '-')) {
        map->hashType = _parse_ht_str(&buf[2]);

        /** set up for fingerprint */
        len = sizeof(buf);
        tmp = buf;
        line = read_config_read_octet_string(line, (u_char **)&tmp, &len);
    }
    else
        map->hashType = NS_HASH_SHA1;
    netsnmp_fp_lowercase_and_strip_colon(buf);
    map->fingerprint = strdup(buf);
    if (NULL == line) {
        netsnmp_config_error(CONFIG_TOKEN  ":must specify map type");
        goto end;
    }

    len = sizeof(buf);
    tmp = buf;
    line = read_config_read_octet_string(line, (u_char **)&tmp, &len);
    if ((buf[0] != '-') || (buf[1] != '-')) {
        netsnmp_config_error(CONFIG_TOKEN ": unexpected fromat: %s\n", line);
        goto end;
    }
    if (strcmp(&buf[2], "sn") == 0) {
        if (NULL == line) {
            netsnmp_config_error(CONFIG_TOKEN
                                 ": must specify secName for --sn");
            goto end;
        }
        len = sizeof(buf);
        tmp = buf;
        line = read_config_read_octet_string(line, (u_char **)&tmp, &len);
        map->data = strdup(buf);
        if (map->data)
            map->mapType = TSNM_tlstmCertSpecified;
    }
    else if (strcmp(&buf[2], "cn") == 0)
        map->mapType = TSNM_tlstmCertCommonName;
    else if (strcmp(&buf[2], "ip") == 0)
        map->mapType = TSNM_tlstmCertSANIpAddress;
    else if (strcmp(&buf[2], "rfc822") == 0)
        map->mapType = TSNM_tlstmCertSANRFC822Name;
    else if (strcmp(&buf[2], "dns") == 0)
        map->mapType = TSNM_tlstmCertSANDNSName;
    else if (strcmp(&buf[2], "any") == 0)
        map->mapType = TSNM_tlstmCertSANAny;
    else
        netsnmp_config_error(CONFIG_TOKEN ": unknown argument %s\n", buf);
    
    map->hashType = NS_HASH_SHA1;

  end:
    if (0 == map->mapType)
        netsnmp_cert_map_free(map);
    else {
        DEBUGMSGT(("cert:util:config", "inserting type %d map, pri %d fp %s\n",
                   map->mapType, map->priority, map->fingerprint));
        if (CONTAINER_INSERT(_maps, map) != 0) 
            netsnmp_config_error(CONFIG_TOKEN
                                 ": duplicate priority for certificate map");
    }
}

static int
_save_map(netsnmp_cert_map *map, void *type)
{
    /** xxx-rks */
    return SNMP_ERR_NOERROR;
}

static int
_save_maps(int majorID, int minorID, void *serverarg, void *clientarg)
{
    char            sep[] =
        "\n##############################################################";
    char            buf[] =
        "#\n" "# certificate secName mapping persistent data\n" "#";
    char           *type = netsnmp_ds_get_string(NETSNMP_DS_LIBRARY_ID,
                                                 NETSNMP_DS_LIB_APPTYPE);

    read_config_store((char *) type, sep);
    read_config_store((char *) type, buf);

    /*
     * save all rows
     */
    CONTAINER_FOR_EACH((netsnmp_container *) _maps,
                       (netsnmp_container_obj_func *)_save_map, type);

    read_config_store((char *) type, sep);
    read_config_store((char *) type, "\n");

    /*
     * never fails 
     */
    return SNMPERR_SUCCESS;
}


/* ***************************************************************************
 *
 * xxx
 *
 */
/** find all entries matching given fingerprint */
static netsnmp_void_array *
_find_subset_fp(netsnmp_container *certs, const char *fp)
{
    netsnmp_cert_map    entry;
    netsnmp_container  *fp_container;
    netsnmp_void_array *va;

    if ((NULL == certs) || (NULL == fp))
        return NULL;

    fp_container = SUBCONTAINER_FIND(certs, "cert2sn_fp");
    netsnmp_assert_or_msgreturn(fp_container, "cert2sn_fp container missing",
                                NULL);

    memset(&entry, 0x0, sizeof(entry));

    entry.fingerprint = NETSNMP_REMOVE_CONST(char*,fp);

    va = CONTAINER_GET_SUBSET(fp_container, &entry);
    return va;
}

static int
_fill_cert_map(netsnmp_cert_map *cert_map, netsnmp_cert_map *entry)
{
    DEBUGMSGT(("cert:map:secname", "map: pri %d type %d data %s\n",
               entry->priority, entry->mapType, entry->data));
    cert_map->priority = entry->priority;
    cert_map->mapType = entry->mapType;
    cert_map->hashType = entry->hashType;
    if (entry->data) {
        cert_map->data = strdup(entry->data);
        if (NULL == cert_map->data ) {
            snmp_log(LOG_ERR, "secname map data dup failed\n");
            return -1;
        }
    }

    return 0;
}

/*
 * get secname map(s) for fingerprints
 */
int
netsnmp_cert_get_secname_maps(netsnmp_container *cert_maps)
{
    netsnmp_iterator   *itr;
    netsnmp_cert_map   *cert_map, *new_cert_map, *entry;
    netsnmp_container  *new_maps = NULL;
    netsnmp_void_array *results;
    int                 j;

    if ((NULL == cert_maps) || (CONTAINER_SIZE(cert_maps) == 0))
        return -1;

    DEBUGMSGT(("cert:map:secname", "looking for matches for %" NETSNMP_PRIz "d fingerprints\n",
               CONTAINER_SIZE(cert_maps)));
    
    /*
     * duplicate cert_maps and then iterate over it. That way we can
     * add/remove to cert_maps without distrubing the iterator.
     */
    new_maps = CONTAINER_DUP(cert_maps, NULL, 0);
    if (NULL == new_maps) {
        snmp_log(LOG_ERR, "could not duplicate maps for secname mapping\n");
        return -1;
    }

    itr = CONTAINER_ITERATOR(new_maps);
    if (NULL == itr) {
        snmp_log(LOG_ERR, "could not get iterator for secname mappings\n");
        CONTAINER_FREE(new_maps);
        return -1;
    }
    cert_map = ITERATOR_FIRST(itr);
    for( ; cert_map; cert_map = ITERATOR_NEXT(itr)) {

        results = _find_subset_fp( netsnmp_cert_map_container(),
                                   cert_map->fingerprint );
        if (NULL == results) {
            DEBUGMSGT(("cert:map:secname", "no match for %s\n",
                       cert_map->fingerprint));
            if (CONTAINER_REMOVE(cert_maps, cert_map) != 0)
                goto fail;
            continue;
        }
        DEBUGMSGT(("cert:map:secname", "%" NETSNMP_PRIz "d matches for %s\n",
                   results->size, cert_map->fingerprint));
        /*
         * first entry is a freebie
         */
        entry = (netsnmp_cert_map*)results->array[0];
        if (_fill_cert_map(cert_map, entry) != 0)
            goto fail;

        /*
         * additional entries must be allocated/inserted
         */
        if (results->size > 1) {
            for(j=1; j < results->size; ++j) {
                entry = (netsnmp_cert_map*)results->array[j];
                new_cert_map = netsnmp_cert_map_alloc(entry->fingerprint,
                                                      entry->ocert);
                if (NULL == new_cert_map) {
                    snmp_log(LOG_ERR,
                             "could not allocate new cert map entry\n");
                    goto fail;
                }
                if (_fill_cert_map(new_cert_map, entry) != 0) {
                    netsnmp_cert_map_free(new_cert_map);
                    goto fail;
                }
                new_cert_map->ocert = cert_map->ocert;
                if (CONTAINER_INSERT(cert_maps,new_cert_map) != 0) {
                    netsnmp_cert_map_free(new_cert_map);
                    goto fail;
                }
            } /* for results */
        } /* results size > 1 */

        free(results->array);
        SNMP_FREE(results);
    }
    ITERATOR_RELEASE(itr);
    CONTAINER_FREE(new_maps);

    DEBUGMSGT(("cert:map:secname", "found %" NETSNMP_PRIz "d matches for fingerprints\n",
               CONTAINER_SIZE(cert_maps)));
    return 0;

  fail:
    if (results) {
        free(results->array);
        free(results);
    }
    ITERATOR_RELEASE(itr);
    CONTAINER_FREE(new_maps);
    return -1;
}

#endif /* defined(NETSNMP_USE_OPENSSL) && defined(HAVE_LIBSSL) */
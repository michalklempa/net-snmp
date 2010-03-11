#ifndef SNMP_LOGGING_H
#define SNMP_LOGGING_H

#ifdef __cplusplus
extern          "C" {
#endif

#if HAVE_SYSLOG_H
#include <syslog.h>
#endif
#include <stdio.h>
#include <stdarg.h>

#ifndef LOG_ERR
#define LOG_EMERG       0       /* system is unusable */
#define LOG_ALERT       1       /* action must be taken immediately */
#define LOG_CRIT        2       /* critical conditions */
#define LOG_ERR         3       /* error conditions */
#define LOG_WARNING     4       /* warning conditions */
#define LOG_NOTICE      5       /* normal but significant condition */
#define LOG_INFO        6       /* informational */
#define LOG_DEBUG       7       /* debug-level messages */
#endif

    struct snmp_log_message {
        int             priority;
        const char     *msg;
    };

#ifndef DEFAULT_LOG_ID
#define DEFAULT_LOG_ID "net-snmp"
#endif

#define NETSNMP_LOGONCE(x) do { \
        static char logged = 0; \
        if (!logged) {          \
            logged = 1;         \
            snmp_log x ;        \
        }                       \
    } while(0)

    void            init_snmp_logging(void);
    void            shutdown_snmp_logging(void);
    NETSNMP_IMPORT
    int             snmp_get_do_logging(void);
    NETSNMP_IMPORT
    void            snmp_disable_syslog(void);
    void            snmp_disable_filelog(void);
    NETSNMP_IMPORT
    void            snmp_disable_stderrlog(void);
    void            snmp_disable_calllog(void);
    NETSNMP_IMPORT
    void            snmp_disable_log(void);
    NETSNMP_IMPORT
    void            snmp_enable_syslog(void);
    NETSNMP_IMPORT
    void            snmp_enable_syslog_ident(const char *ident,
                                             const int   facility);
    NETSNMP_IMPORT
    void            snmp_enable_filelog(const char *logfilename,
                                        int dont_zero_log);
    NETSNMP_IMPORT
    void            snmp_enable_stderrlog(void);
    void            snmp_enable_calllog(void);

    NETSNMP_IMPORT
    int             snmp_stderrlog_status(void);

#if !defined(__GNUC__) || __GNUC__ < 2 || (__GNUC__ == 2 && __GNUC_MINOR__ < 8)
    NETSNMP_IMPORT
    int             snmp_log(int priority, const char *format, ...);
#else
    NETSNMP_IMPORT
    int             snmp_log(int priority, const char *format, ...)
                    	__attribute__ ((__format__ (__printf__, 2, 3)));
#endif
    int             snmp_vlog(int priority, const char *format,
                              va_list ap);
    /*
     * 0 - successful message formatting 
     * -1 - Could not format log-string 
     * -2 - Could not allocate memory for log-message 
     * -3 - Log-message too long! 
     */

    NETSNMP_IMPORT
    void            snmp_log_perror(const char *s);


#define NETSNMP_LOGHANDLER_STDOUT	1
#define NETSNMP_LOGHANDLER_STDERR	2
#define NETSNMP_LOGHANDLER_FILE		3
#define NETSNMP_LOGHANDLER_SYSLOG	4
#define NETSNMP_LOGHANDLER_CALLBACK	5
#define NETSNMP_LOGHANDLER_NONE		6

    NETSNMP_IMPORT
    void netsnmp_set_line_buffering(FILE *stream);
    NETSNMP_IMPORT
    int snmp_log_options(char *optarg, int argc, char *const *argv);
    NETSNMP_IMPORT
    void snmp_log_options_usage(const char *lead, FILE *outf);
    NETSNMP_IMPORT
    char *snmp_log_syslogname(const char *syslogname);
    typedef struct netsnmp_log_handler_s netsnmp_log_handler; 
    typedef int (NetsnmpLogHandler)(netsnmp_log_handler*, int, const char *);

    NetsnmpLogHandler log_handler_stdouterr;
    NetsnmpLogHandler log_handler_file;
    NetsnmpLogHandler log_handler_syslog;
    NetsnmpLogHandler log_handler_callback;
    NetsnmpLogHandler log_handler_null;

    struct netsnmp_log_handler_s {
        int	enabled;
        int	priority;
        int	pri_max;
        int	type;
	const char *token;		/* Also used for filename */

	NetsnmpLogHandler	*handler;

	int     imagic;		/* E.g. file descriptor, syslog facility */
	void   *magic;		/* E.g. Callback function */

	netsnmp_log_handler	*next, *prev;
    };

NETSNMP_IMPORT
netsnmp_log_handler *get_logh_head( void );
NETSNMP_IMPORT
netsnmp_log_handler *netsnmp_register_loghandler( int type, int pri );
netsnmp_log_handler *netsnmp_find_loghandler( const char *token );
int netsnmp_add_loghandler(    netsnmp_log_handler *logh );
NETSNMP_IMPORT
int netsnmp_remove_loghandler( netsnmp_log_handler *logh );
int netsnmp_enable_loghandler( const char *token );
int netsnmp_disable_loghandler( const char *token );
NETSNMP_IMPORT
void netsnmp_logging_restart(void);
#ifdef __cplusplus
}
#endif
#endif                          /* SNMP_LOGGING_H */
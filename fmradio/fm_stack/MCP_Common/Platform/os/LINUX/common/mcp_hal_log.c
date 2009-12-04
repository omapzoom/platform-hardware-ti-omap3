/******************************************************************************\
##                                                                            *
## Unpublished Proprietary and Confidential Information of Texas Instruments  *
## Israel Ltd. Do Not Disclose.                                               *
## Copyright 2008 Texas Instruments Israel Ltd.                               *
## All rights reserved. All unpublished rights reserved.                      *
##                                                                            *
## No part of this work may be used or reproduced in any form or by any       *
## means, or stored in a database or retrieval system, without prior written  *
## permission of Texas Instruments Israel Ltd. or its parent company Texas    *
## Instruments Incorporated.                                                  *
## Use of this work is subject to a license from Texas Instruments Israel     *
## Ltd. or its parent company Texas Instruments Incorporated.                 *
##                                                                            *
## This work contains Texas Instruments Israel Ltd. confidential and          *
## proprietary information which is protected by copyright, trade secret,     *
## trademark and other intellectual property rights.                          *
##                                                                            *
## The United States, Israel  and other countries maintain controls on the    *
## export and/or import of cryptographic items and technology. Unless prior   *
## authorization is obtained from the U.S. Department of Commerce and the     *
## Israeli Government, you shall not export, reexport, or release, directly   *
## or indirectly, any technology, software, or software source code received  *
## from Texas Instruments Incorporated (TI) or Texas Instruments Israel,      *
## or export, directly or indirectly, any direct product of such technology,  *
## software, or software source code to any destination or country to which   *
## the export, reexport or release of the technology, software, software      *
## source code, or direct product is prohibited by the EAR. The subject items *
## are classified as encryption items under Part 740.17 of the Commerce       *
## Control List (CCL). The assurances provided for herein are furnished in  *
## compliance with the specific encryption controls set forth in Part 740.17  *
## of the EAR -Encryption Commodities and Software (ENC).                     *
##                                                                            *
## NOTE: THE TRANSFER OF THE TECHNICAL INFORMATION IS BEING MADE UNDER AN     *
## EXPORT LICENSE ISSUED BY THE ISRAELI GOVERNMENT AND THE APPLICABLE EXPORT  *
## LICENSE DOES NOT ALLOW THE TECHNICAL INFORMATION TO BE USED FOR THE        *
## MODIFICATION OF THE BT ENCRYPTION OR THE DEVELOPMENT OF ANY NEW ENCRYPTION.*
## UNDER THE ISRAELI GOVERNMENT'S EXPORT LICENSE, THE INFORMATION CAN BE USED *
## FOR THE INTERNAL DESIGN AND MANUFACTURE OF TI PRODUCTS THAT WILL CONTAIN   *
## THE BT IC.                                                                 *
##                                                                            *
\******************************************************************************/
/*******************************************************************************\
*
*   FILE NAME:      mcp_hal_log.c
*
*   DESCRIPTION:    This file implements the API of the MCP HAL log utilities.
*
*   AUTHOR:         Chen Ganir
*
\*******************************************************************************/


/********************************************************************************
 *
 * Include files
 *
 *******************************************************************************/
#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/syscall.h>

#include <stdio.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>


#include "mcp_hal_types.h"
#include "mcp_hal_log.h"
#include "mcp_hal_log_udp.h"
#include "mcp_hal_config.h"
#include "mcp_hal_string.h"
#include "mcp_hal_memory.h"
#include "mcp_defs.h"

#define APP_NAME_MAX_LEN    50

#ifdef ANDROID
#include "cutils/log.h"
#undef LOG_TAG
#define LOG_TAG gMcpLogAppName
#endif

void MCP_HAL_InitUdpSockets(void);

/****************************************************************************
 *
 * Constants
 *
 ****************************************************************************/

#define MCP_HAL_MAX_FORMATTED_MSG_LEN           (200)
#define MCP_HAL_MAX_USER_MSG_LEN                (100)
#define BUFFSIZE 255

static char _mcpLog_FormattedMsg[MCP_HAL_MAX_FORMATTED_MSG_LEN + 1];

McpU8 gMcpLogEnabled = 0;
McpU8 gMcpLogToStdout = 0;
McpU8 gMcpLogToFile = 0;
McpU8 gMcpLogToUdpSocket = 0;

#ifdef ANDROID
McpU8 gMcpLogToAndroid = 0;
char gMcpLogAppName[APP_NAME_MAX_LEN] = {0};
#endif

char gMcpLogUdpTargetAddress[30] = "";
unsigned long gMcpLogUdpTargetPort = 0;
char gMcpLogFileName[512] = "";
int g_udp_sock = -1;

time_t g_start_time_seconds = 0;

struct sockaddr_in g_udp_logserver;

static pthread_key_t thread_id;

static McpU8 gInitialized = 0;

FILE* log_output_handle = NULL;

/* this key will hold the thread-specific thread handle that will be used
 * for self identification of threads. the goal is that every thread will know its name
 * to ease having a consistent self identifying logs */
static pthread_key_t thread_name;

#ifdef ANDROID
void MCP_HAL_LOG_EnableLogToAndroid(const char *app_name)
{
    gMcpLogToAndroid = 1;
    gMcpLogEnabled = 1;
    strncpy(gMcpLogAppName,app_name,APP_NAME_MAX_LEN-1);
}
#endif

void MCP_HAL_LOG_EnableUdpLogging(const char* ip, unsigned long port)
{
    gMcpLogEnabled = 1;
    strcpy(gMcpLogUdpTargetAddress,ip);
    gMcpLogUdpTargetPort = port;
    gMcpLogToUdpSocket = 1;
    MCP_HAL_InitUdpSockets();
}

void MCP_HAL_LOG_EnableFileLogging(const char* fileName)
{
    gMcpLogEnabled = 1;
    strcpy(gMcpLogFileName,fileName); 
    gMcpLogToStdout = 0; 
    gMcpLogToFile = 1; 

    log_output_handle = fopen(gMcpLogFileName,"at");
    if (log_output_handle == NULL)
    {
        fprintf(stderr, "MCP_HAL_LOG_LogMsg failed to open '%s' for writing",gMcpLogFileName);
        gMcpLogEnabled = 0;
        return;
    }    
}

void MCP_HAL_LOG_EnableStdoutLogging(void)
{
    gMcpLogEnabled = 1;
    gMcpLogToStdout = 1; 
    gMcpLogToFile = 0; 
    strcpy(gMcpLogFileName,(char*)"");
    log_output_handle = stdout;
}

static unsigned int MCP_HAL_LOG_GetThreadId(void)
{
    #ifndef SYS_gettid      //should be normally defined on standard Linux
    #define SYS_gettid 224 //224 on android
    #endif

    return syscall(SYS_gettid);
}

static void MCP_HAL_LOG_SetThreadIdName(unsigned int id, const char *name)
{
    int rc;
    rc = pthread_setspecific(thread_id, (void *)id);
    if (0 != rc)
        fprintf(stderr, "MCP_HAL_LOG_SetThreadIdName | pthread_setspecific() (id) failed: %s", strerror(rc));

    rc = pthread_setspecific(thread_name, name);
    if (0 != rc)
        fprintf(stderr, "MCP_HAL_LOG_SetThreadIdName | pthread_setspecific() (handle) failed: %s", strerror(rc));
}

static char *MCP_HAL_LOG_GetThreadName(void)
{
    return pthread_getspecific(thread_name);
}

void MCP_HAL_LOG_SetThreadName(const char* name)
{
    MCP_HAL_LOG_SetThreadIdName(MCP_HAL_LOG_GetThreadId(), name);
}

/*-------------------------------------------------------------------------------
 * MCP_HAL_LOG_FormatMsg()
 *
 *      sprintf-like string formatting. the formatted string is allocated by the function.
 *
 * Type:
 *      Synchronous, non-reentrant 
 *
 * Parameters:
 *
 *      format [in]- format string
 *
 *      ...     [in]- additional format arguments
 *
 * Returns:
 *     Returns pointer to the formatted string
 *
 */
char *MCP_HAL_LOG_FormatMsg(const char *format, ...)
{
    va_list     args;

    _mcpLog_FormattedMsg[MCP_HAL_MAX_FORMATTED_MSG_LEN] = '\0';

    va_start(args, format);

    vsnprintf(_mcpLog_FormattedMsg, MCP_HAL_MAX_FORMATTED_MSG_LEN, format, args);

    va_end(args);

    return _mcpLog_FormattedMsg;
}

void MCP_HAL_InitUdpSockets(void)
{
    /* Initialize the socket */
    if (gMcpLogToUdpSocket == 1 && g_udp_sock < 0 )
    {
        /* Create the UDP socket */
        if ((g_udp_sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) >= 0) 
        {
            /* Construct the server sockaddr_in structure */
            memset(&g_udp_logserver, 0, sizeof(g_udp_logserver)); /* Clear struct */
            g_udp_logserver.sin_family = AF_INET; /* Internet/IP */
            g_udp_logserver.sin_addr.s_addr = inet_addr(gMcpLogUdpTargetAddress); /* IP address */
            g_udp_logserver.sin_port = htons(gMcpLogUdpTargetPort); /* server port */    
        }            
    }
}

void MCP_HAL_DeInitUdpSockets(void)
{
    /* Deinitialize the socket */
    if (gMcpLogToUdpSocket == 1 && g_udp_sock >=0 )
    {
        close(g_udp_sock);
        g_udp_sock = -1;
    }
}

void MCP_HAL_LOG_Init(void)
{
    int rc;
    struct timeval detail_time;

    if (gInitialized == 0)
    {
        /* Query for current time */
        gettimeofday(&detail_time,NULL);
        g_start_time_seconds = detail_time.tv_sec;

        rc = pthread_key_create(&thread_id, NULL);
        if(0 != rc)
            fprintf(stderr, "MCP_HAL_LogInit | pthread_key_create() thread_id failed: %s", strerror(rc));

        rc = pthread_key_create(&thread_name, NULL);
        if(0 != rc)
            fprintf(stderr, "MCP_HAL_LogInit | pthread_key_create() thread_name failed: %s", strerror(rc));

        gInitialized = 1;
    }
}

void MCP_HAL_LOG_Deinit(void)
{
    int rc;

    MCP_HAL_DeInitUdpSockets();

    rc = pthread_key_delete(thread_id);
    if (0 != rc)
        fprintf(stderr, "MCP_HAL_LogDeinit | pthread_key_delete() failed: %s", strerror(rc));

    rc = pthread_key_delete(thread_name);
    if (0 != rc)
        fprintf(stderr, "MCP_HAL_LogDeinit | pthread_key_delete() failed: %s", strerror(rc));
}

const char *MCP_HAL_LOG_SeverityCodeToName(McpHalLogSeverity Severity)
{
   switch(Severity)
    {       
        case (MCP_HAL_LOG_SEVERITY_FUNCTION):
            return ("FUNCT");
        case (MCP_HAL_LOG_SEVERITY_DEBUG):
            return ("DEBUG");
        case (MCP_HAL_LOG_SEVERITY_INFO):
            return ("INFO");
        case (MCP_HAL_LOG_SEVERITY_ERROR):
            return ("ERROR");
        case (MCP_HAL_LOG_SEVERITY_FATAL):
            return ("FATAL");
        default:
            return (" ");
    }
    return (" ");
}


static void MCP_HAL_LOG_LogToFile(const char*       fileName, 
                           McpU32           line, 
                           McpHalLogModuleId_e moduleId,
                           McpHalLogSeverity severity,  
                           const char*      msg)
{
    struct timeval detail_time;
    char copy_of_msg[MCP_HAL_MAX_USER_MSG_LEN+1] = "";
    char log_formatted_str[MCP_HAL_MAX_FORMATTED_MSG_LEN+1] = "";
    size_t copy_of_msg_len = 0;
    char* moduleName = NULL;

    /* Remove any CR at the end of the user message */
    strncpy(copy_of_msg,msg,MCP_HAL_MAX_USER_MSG_LEN);
    copy_of_msg_len = strlen(copy_of_msg);
    if (copy_of_msg[copy_of_msg_len-1] == '\n')
        copy_of_msg[copy_of_msg_len-1] = ' ';

    /* Get the thread name */
    char *threadName = MCP_HAL_LOG_GetThreadName();
    
    /* Query for current time */
    gettimeofday(&detail_time,NULL);

    /* Get the module name */
    moduleName = MCP_HAL_LOG_Modules[moduleId].name;

    /* Format the final log message to be printed */
    snprintf(log_formatted_str,MCP_HAL_MAX_FORMATTED_MSG_LEN,
              "%06ld.%06ld|%-5s|%-15s|%s|%s {%s@%ld}\n",
              detail_time.tv_sec - g_start_time_seconds,
              detail_time.tv_usec,
              MCP_HAL_LOG_SeverityCodeToName(severity),
              moduleName,
              (threadName != NULL ? threadName: "UNKNOWN"),
              copy_of_msg,
              (fileName==NULL?"UNKOWN":fileName),
              line);

    /* Write the formatted final message to the file */
    fwrite(log_formatted_str, 1, MCP_HAL_STRING_StrLen(log_formatted_str), log_output_handle);
    fflush(log_output_handle);
}

static void MCP_HAL_LOG_LogToUdp(const char*        fileName, 
                          McpU32            line, 
                          McpHalLogModuleId_e moduleId,
                          McpHalLogSeverity severity,  
                          const char*       msg)
{
    udp_log_msg_t udp_msg;
    char *threadName = MCP_HAL_LOG_GetThreadName();
    size_t logStrLen = 0;

    /* Reset log message */  
    memset(&udp_msg,0,sizeof(udp_log_msg_t));

    /* Copy user message */
    strncpy(udp_msg.message,msg,MCPHAL_LOG_MAX_MESSAGE_LENGTH-1);
    logStrLen = strlen(udp_msg.message);
    if (udp_msg.message[logStrLen-1] == '\n')
        udp_msg.message[logStrLen-1] = ' ';

    /* Copy file name */
    strncpy(udp_msg.fileName,(fileName==NULL ? "MAIN":fileName),MCPHAL_LOG_MAX_FILENAME_LENGTH-1);

    /* Copy thread name */
    strncpy(udp_msg.threadName,(threadName!=NULL ? MCP_HAL_LOG_GetThreadName() : "UNKNOWN"),MCPHAL_LOG_MAX_THREADNAME_LENGTH-1);

    /* Copy other relevant log information */    
    udp_msg.line = line;
    udp_msg.moduleId = moduleId;
    udp_msg.severity = severity;

    /* Write log message to socket */
    (void)sendto(g_udp_sock, &udp_msg, sizeof(udp_msg), 0,(struct sockaddr *) &g_udp_logserver,sizeof(g_udp_logserver));
}
    

#ifdef ANDROID    

static void MCP_HAL_LOG_LogToAndroid(const char*        fileName, 
                          McpU32            line, 
                          McpHalLogModuleId_e moduleId,
                          McpHalLogSeverity severity,  
                          const char*       msg)
{
    char copy_of_msg[MCP_HAL_MAX_USER_MSG_LEN+1] = "";
    size_t copy_of_msg_len = 0;
    char *threadName = MCP_HAL_LOG_GetThreadName();

    strncpy(copy_of_msg,msg,MCP_HAL_MAX_USER_MSG_LEN);
    copy_of_msg_len = strlen(copy_of_msg);
    if (copy_of_msg[copy_of_msg_len-1] == '\n')
        copy_of_msg[copy_of_msg_len-1] = ' ';
    
    switch (severity)
    {
        case MCP_HAL_LOG_SEVERITY_FUNCTION:
            LOGV("%s(%s):%s (%ld@%s)",MCP_HAL_LOG_Modules[moduleId].name,
                threadName,
                copy_of_msg,
                line,
                fileName);
            break;
        case MCP_HAL_LOG_SEVERITY_DEBUG:   
            LOGD("%s(%s):%s (%ld@%s)",MCP_HAL_LOG_Modules[moduleId].name,
                threadName,
                copy_of_msg,
                line,
                fileName);
            break;
        case MCP_HAL_LOG_SEVERITY_INFO:   
            LOGI("%s(%s):%s (%ld@%s)",MCP_HAL_LOG_Modules[moduleId].name,
                threadName,
                copy_of_msg,
                line,
                fileName);
            break;
        case MCP_HAL_LOG_SEVERITY_ERROR:  
            LOGE("%s(%s):%s (%ld@%s)",MCP_HAL_LOG_Modules[moduleId].name,
                MCP_HAL_LOG_GetThreadName(),
                copy_of_msg,
                line,
                fileName);
            break;
        case MCP_HAL_LOG_SEVERITY_FATAL:
            LOGE("%s(%s):%s (%ld@%s)",MCP_HAL_LOG_Modules[moduleId].name,
                threadName,
                copy_of_msg,
                line,
                fileName);
            break;
        default:
            break;
    }
}
#endif

/*-------------------------------------------------------------------------------
 * MCP_HAL_LOG_LogMsg()
 *
 *      Sends a log message to the local logging tool.
 *      Not all parameters are necessarily supported in all platforms.
 *     
 * Type:
 *      Synchronous
 *
 * Parameters:
 *
 *      fileName [in] - name of file originating the message
 *
 *      line [in] - line number in the file
 *
 *      moduleType [in] - e.g. "BTL_BMG", "BTL_SPP", "BTL_OPP", 
 *
 *      severity [in] - debug, error...
 *
 *      msg [in] - message in already formatted string
 *
 *  Returns:
 *      void
 * 
 */
void MCP_HAL_LOG_LogMsg(    const char*     fileName, 
                                McpU32          line, 
                                McpHalLogModuleId_e moduleId,
                                McpHalLogSeverity severity,  
                                const char*         msg)
{
    if (gMcpLogEnabled && gInitialized == 1)
    {
        /* Log to file or stdout */
        if ( NULL != log_output_handle )
            MCP_HAL_LOG_LogToFile(fileName,line,moduleId,severity,msg);

        /* Log to UDP (textual) */
        if ( 1 == gMcpLogToUdpSocket )
            MCP_HAL_LOG_LogToUdp(fileName,line,moduleId,severity,msg);

#ifdef ANDROID
        if ( 1 == gMcpLogToAndroid )
            MCP_HAL_LOG_LogToAndroid(fileName,line,moduleId,severity,msg);
#endif
    }
}



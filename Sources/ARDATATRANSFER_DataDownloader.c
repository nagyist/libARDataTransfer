/**
 * @file ARDATATRANSFER_DataManager.c
 * @brief libARDataTransfer DataManager c file.
 * @date 19/12/2013
 * @author david.flattin.ext@parrot.com
 **/

#include "config.h"
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#ifdef HAVE_SYS_STATFS_H
#include <sys/statfs.h> //linux
#endif
#ifdef HAVE_SYS_MOUNT_H
#include <sys/mount.h> //ios
#endif

#include <libARSAL/ARSAL_Sem.h>
#include <libARSAL/ARSAL_Mutex.h>
#include <libARSAL/ARSAL_Print.h>
#include <libARSAL/ARSAL_Ftw.h>
#include <libARUtils/ARUTILS_Error.h>
#include <libARUtils/ARUTILS_Ftp.h>
#include <libARUtils/ARUTILS_FileSystem.h>

#include "libARDataTransfer/ARDATATRANSFER_Error.h"
#include "libARDataTransfer/ARDATATRANSFER_Manager.h"
#include "libARDataTransfer/ARDATATRANSFER_DataDownloader.h"
#include "libARDataTransfer/ARDATATRANSFER_MediasDownloader.h"
#include "ARDATATRANSFER_MediasQueue.h"
#include "ARDATATRANSFER_DataDownloader.h"
#include "ARDATATRANSFER_MediasDownloader.h"
#include "ARDATATRANSFER_Manager.h"


#define ARDATATRANSFER_DATA_DOWNLOADER_TAG                  "DataDownloader"

#define ARDATATRANSFER_DATA_DOWNLOADER_WAIT_TIME_IN_SECONDS   10
#define ARDATATRANSFER_DATA_DOWNLOADER_FTP_ROOT              "data"
#define ARDATATRANSFER_DATA_DOWNLOADER_SPACE_PERCENT          20.f

static ARDATATRANSFER_DataDownloader_Fwt_t dataFwt;

/*****************************************
 *
 *             Public implementation:
 *
 *****************************************/

eARDATATRANSFER_ERROR ARDATATRANSFER_DataDownloader_New(ARDATATRANSFER_Manager_t *manager, const char *deviceIP, int port, const char *localDirectory)
{
    eARDATATRANSFER_ERROR result = ARDATATRANSFER_OK;
    int resultSys = 0;
    
    ARSAL_PRINT(ARSAL_PRINT_DEBUG, ARDATATRANSFER_DATA_DOWNLOADER_TAG, "");
    
    if (manager == NULL)
    {
        result = ARDATATRANSFER_ERROR_BAD_PARAMETER;
    }
    
    if (result == ARDATATRANSFER_OK)
    {
        if (manager->dataDownloader != NULL)
        {
            result = ARDATATRANSFER_ERROR_ALREADY_INITIALIZED;
        }
        else
        {
            manager->dataDownloader = (ARDATATRANSFER_DataDownloader_t *)calloc(1, sizeof(ARDATATRANSFER_DataDownloader_t));
            
            if (manager->dataDownloader == NULL)
            {
                result = ARDATATRANSFER_ERROR_ALLOC;
            }
        }
    }
    
    if (result == ARDATATRANSFER_OK)
    {
        resultSys = ARSAL_Sem_Init(&manager->dataDownloader->threadSem, 0, 0);
        
        if (resultSys != 0)
        {
            result = ARDATATRANSFER_ERROR_SYSTEM;
        }
    }
    
    if (result == ARDATATRANSFER_OK)
    {
        manager->dataDownloader->isCanceled = 0;
        manager->dataDownloader->isRunning = 0;
        
        result = ARDATATRANSFER_DataDownloader_Initialize(manager, deviceIP, port, localDirectory);
    }
    
    if (result != ARDATATRANSFER_OK && result != ARDATATRANSFER_ERROR_ALREADY_INITIALIZED)
    {
        ARDATATRANSFER_DataDownloader_Delete(manager);
    }
    
    return result;
}

eARDATATRANSFER_ERROR ARDATATRANSFER_DataDownloader_Delete(ARDATATRANSFER_Manager_t *manager)
{
    eARDATATRANSFER_ERROR result = ARDATATRANSFER_OK;
    
    ARSAL_PRINT(ARSAL_PRINT_DEBUG, ARDATATRANSFER_DATA_DOWNLOADER_TAG, "");
    
    if (manager == NULL)
    {
        result = ARDATATRANSFER_ERROR_BAD_PARAMETER;
    }
    else
    {
        if (manager->dataDownloader == NULL)
        {
            result = ARDATATRANSFER_ERROR_NOT_INITIALIZED;
        }
        else
        {
            if (manager->dataDownloader->isRunning != 0)
            {
                result = ARDATATRANSFER_ERROR_THREAD_PROCESSING;
            }
            else
            {
                ARDATATRANSFER_DataDownloader_CancelThread(manager);
                
                ARDATATRANSFER_DataDownloader_Clear(manager);
                
                ARSAL_Sem_Destroy(&manager->dataDownloader->threadSem);
                
                free(manager->dataDownloader);
                manager->dataDownloader = NULL;
            }
        }
    }
    
    return result;
}

void* ARDATATRANSFER_DataDownloader_ThreadRun(void *managerArg)
{
    ARDATATRANSFER_Manager_t *manager = (ARDATATRANSFER_Manager_t *)managerArg;
    eARDATATRANSFER_ERROR result = ARDATATRANSFER_OK;
    int resultSys = 0;

    ARSAL_PRINT(ARSAL_PRINT_DEBUG, ARDATATRANSFER_DATA_DOWNLOADER_TAG, "");

    if (manager == NULL)
    {
        result = ARDATATRANSFER_ERROR_BAD_PARAMETER;
    }

    if ((result == ARDATATRANSFER_OK) && (manager->dataDownloader == NULL))
    {
        result = ARDATATRANSFER_ERROR_NOT_INITIALIZED;
    }
    
    if (result == ARDATATRANSFER_OK && (manager->dataDownloader->isCanceled != 0))
    {
        result = ARDATATRANSFER_ERROR_CANCELED;
    }

    if ((result == ARDATATRANSFER_OK) && (manager->dataDownloader->isRunning != 0))
    {
        result = ARDATATRANSFER_ERROR_THREAD_ALREADY_RUNNING;
    }

    if (result == ARDATATRANSFER_OK)
    {
        manager->dataDownloader->isRunning = 1;
    }

    if (result == ARDATATRANSFER_OK)
    {
        struct timespec timeout;
        timeout.tv_sec = ARDATATRANSFER_DATA_DOWNLOADER_WAIT_TIME_IN_SECONDS;
        timeout.tv_nsec = 0;

        do
        {
            char remotePath[ARUTILS_FTP_MAX_PATH_SIZE];
            char localPath[ARUTILS_FTP_MAX_PATH_SIZE];
            eARUTILS_ERROR errorFtp = ARUTILS_OK;
            eARUTILS_ERROR error = ARUTILS_OK;
            char *resultList = NULL;
            uint32_t resultListLen = 0;
            const char *nextItem = NULL;
            const char *fileName;

            error = ARUTILS_Ftp_List(manager->dataDownloader->ftp, ARDATATRANSFER_DATA_DOWNLOADER_FTP_ROOT, &resultList, &resultListLen);

            // Resume downloading_ files loop
            nextItem = NULL;
            
            while ((error == ARUTILS_OK)
                   && (fileName = ARDATATRANSFER_Manager_List_GetNextItem(resultList, &nextItem, ARDATATRANSFER_MANAGER_DOWNLOADER_PREFIX_DOWNLOADING, 0)) != NULL)
            {
                char restoreName[ARUTILS_FTP_MAX_PATH_SIZE];

                strncpy(remotePath, ARDATATRANSFER_DATA_DOWNLOADER_FTP_ROOT "/", ARUTILS_FTP_MAX_PATH_SIZE);
                remotePath[ARUTILS_FTP_MAX_PATH_SIZE - 1] = '\0';
                strncat(remotePath, fileName, ARUTILS_FTP_MAX_PATH_SIZE - strlen(remotePath) - 1);
                strncpy(localPath, manager->dataDownloader->localDirectory, ARUTILS_FTP_MAX_PATH_SIZE);
                localPath[ARUTILS_FTP_MAX_PATH_SIZE - 1] = '\0';
                strncat(localPath, "/" ARDATATRANSFER_DATA_DOWNLOADER_FTP_ROOT "/", ARUTILS_FTP_MAX_PATH_SIZE - strlen(localPath) - 1);
                strncat(localPath, fileName, ARUTILS_FTP_MAX_PATH_SIZE - strlen(localPath) -1);

                errorFtp = ARUTILS_Ftp_Get(manager->dataDownloader->ftp, remotePath, localPath, NULL, NULL, FTP_RESUME_TRUE);

                if (errorFtp == ARUTILS_OK)
                {
                    errorFtp = ARUTILS_Ftp_Delete(manager->dataDownloader->ftp, remotePath);

                    strncpy(restoreName, manager->dataDownloader->localDirectory, ARUTILS_FTP_MAX_PATH_SIZE);
                    restoreName[ARUTILS_FTP_MAX_PATH_SIZE - 1] = '\0';
                    strncat(restoreName, "/" ARDATATRANSFER_DATA_DOWNLOADER_FTP_ROOT "/", ARUTILS_FTP_MAX_PATH_SIZE - strlen(restoreName) - 1);
                    strncat(restoreName, fileName + strlen(ARDATATRANSFER_MANAGER_DOWNLOADER_PREFIX_DOWNLOADING), ARUTILS_FTP_MAX_PATH_SIZE - strlen(restoreName) - 1);

                    errorFtp = ARUTILS_FileSystem_Rename(localPath, restoreName);
                }
            }

            // Newer files loop
            nextItem = NULL;
            
            while ((error == ARUTILS_OK)
                   && (fileName = ARDATATRANSFER_Manager_List_GetNextItem(resultList, &nextItem, NULL, 0)) != NULL)
            {
                if (strncmp(fileName, ARDATATRANSFER_MANAGER_DOWNLOADER_PREFIX_DOWNLOADING, strlen(ARDATATRANSFER_MANAGER_DOWNLOADER_PREFIX_DOWNLOADING)) != 0)
                {
                    char initialPath[ARUTILS_FTP_MAX_PATH_SIZE];
                    char restoreName[ARUTILS_FTP_MAX_PATH_SIZE];

                    strncpy(initialPath, ARDATATRANSFER_DATA_DOWNLOADER_FTP_ROOT "/", ARUTILS_FTP_MAX_PATH_SIZE);
                    initialPath[ARUTILS_FTP_MAX_PATH_SIZE - 1] = '\0';
                    strncat(initialPath, fileName, ARUTILS_FTP_MAX_PATH_SIZE - strlen(initialPath) - 1);
                    strncpy(remotePath, ARDATATRANSFER_DATA_DOWNLOADER_FTP_ROOT "/" ARDATATRANSFER_MANAGER_DOWNLOADER_PREFIX_DOWNLOADING, ARUTILS_FTP_MAX_PATH_SIZE);
                    remotePath[ARUTILS_FTP_MAX_PATH_SIZE - 1] = '\0';
                    strncat(remotePath, fileName, ARUTILS_FTP_MAX_PATH_SIZE - strlen(remotePath) - 1);
                    strncpy(localPath, manager->dataDownloader->localDirectory, ARUTILS_FTP_MAX_PATH_SIZE);
                    localPath[ARUTILS_FTP_MAX_PATH_SIZE - 1] = '\0';
                    strncat(localPath, "/" ARDATATRANSFER_DATA_DOWNLOADER_FTP_ROOT "/" ARDATATRANSFER_MANAGER_DOWNLOADER_PREFIX_DOWNLOADING, ARUTILS_FTP_MAX_PATH_SIZE - strlen(localPath) - 1);
                    strncat(localPath, fileName, ARUTILS_FTP_MAX_PATH_SIZE - strlen(localPath) - 1);

                    errorFtp = ARUTILS_Ftp_Rename(manager->dataDownloader->ftp, initialPath, remotePath);

                    if (errorFtp == ARUTILS_OK)
                    {
                        errorFtp = ARUTILS_Ftp_Get(manager->dataDownloader->ftp, remotePath, localPath, NULL, NULL, FTP_RESUME_FALSE);
                    }

                    if (errorFtp == ARUTILS_OK)
                    {
                        errorFtp = ARUTILS_Ftp_Delete(manager->dataDownloader->ftp, remotePath);

                        strncpy(restoreName, manager->dataDownloader->localDirectory, ARUTILS_FTP_MAX_PATH_SIZE);
                        restoreName[ARUTILS_FTP_MAX_PATH_SIZE - 1] = '\0';
                        strncat(restoreName, "/" ARDATATRANSFER_DATA_DOWNLOADER_FTP_ROOT "/", ARUTILS_FTP_MAX_PATH_SIZE - strlen(restoreName) - 1);
                        strncat(restoreName, fileName, ARUTILS_FTP_MAX_PATH_SIZE - strlen(restoreName) - 1);

                        errorFtp = ARUTILS_FileSystem_Rename(localPath, restoreName);
                    }
                }
            }

            if (resultList != NULL)
            {
                free(resultList);
                resultList = NULL;
                resultListLen = 0;
            }

            if (error != ARUTILS_ERROR_FTP_CANCELED)
            {
                strncpy(localPath, manager->dataDownloader->localDirectory, ARUTILS_FTP_MAX_PATH_SIZE);
                localPath[ARUTILS_FTP_MAX_PATH_SIZE - 1] = '\0';
                strncat(localPath, "/" ARDATATRANSFER_DATA_DOWNLOADER_FTP_ROOT, ARUTILS_FTP_MAX_PATH_SIZE - strlen(localPath) - 1);

                ARDATATRANSFER_DataDownloader_CheckUsedMemory(localPath, ARDATATRANSFER_DATA_DOWNLOADER_SPACE_PERCENT);
            }
            
            resultSys = ARSAL_Sem_Timedwait(&manager->dataDownloader->threadSem, &timeout);
        }
        while ((resultSys == -1) && (errno == ETIMEDOUT));
    }

    if (manager != NULL && manager->dataDownloader != NULL)
    {
        manager->dataDownloader->isRunning = 0;
    }

    ARSAL_PRINT(ARSAL_PRINT_DEBUG, ARDATATRANSFER_DATA_DOWNLOADER_TAG, "exit");

    return NULL;
}

eARDATATRANSFER_ERROR ARDATATRANSFER_DataDownloader_CancelThread(ARDATATRANSFER_Manager_t *manager)
{
    eARDATATRANSFER_ERROR result = ARDATATRANSFER_OK;
    int resultSys = 0;
    
    ARSAL_PRINT(ARSAL_PRINT_DEBUG, ARDATATRANSFER_DATA_DOWNLOADER_TAG, "");
    
    if (manager == NULL)
    {
        result = ARDATATRANSFER_ERROR_BAD_PARAMETER;
    }
    
    if ((result == ARDATATRANSFER_OK) && (manager->dataDownloader == NULL))
    {
        result = ARDATATRANSFER_ERROR_NOT_INITIALIZED;
    }
    
    if (result == ARDATATRANSFER_OK)
    {
        manager->dataDownloader->isCanceled = 1;
        
        resultSys = ARSAL_Sem_Post(&manager->dataDownloader->threadSem);
        
        if (resultSys != 0)
        {
            result = ARDATATRANSFER_ERROR_SYSTEM;
        }
    }
    
    return result;
}

/*****************************************
 *
 *             Private implementation:
 *
 *****************************************/

eARDATATRANSFER_ERROR ARDATATRANSFER_DataDownloader_Initialize(ARDATATRANSFER_Manager_t *manager, const char *deviceIP, int port, const char *localDirectory)
{
    eARDATATRANSFER_ERROR result = ARDATATRANSFER_OK;
    eARUTILS_ERROR error = ARUTILS_OK;
    int resultSys = 0;
    
    ARSAL_PRINT(ARSAL_PRINT_DEBUG, ARDATATRANSFER_DATA_DOWNLOADER_TAG, "%s, %s, %d", deviceIP ? deviceIP : "null", localDirectory ? localDirectory : "null", port);
    
    if ((manager == NULL) || (deviceIP == NULL) || (localDirectory == NULL))
    {
        result = ARDATATRANSFER_ERROR_BAD_PARAMETER;
    }
    
    if (result == ARDATATRANSFER_OK)
    {
        strncpy(manager->dataDownloader->localDirectory, localDirectory, ARUTILS_FTP_MAX_PATH_SIZE);
        manager->dataDownloader->localDirectory[ARUTILS_FTP_MAX_PATH_SIZE - 1] = '\0';
        
        char localPath[ARUTILS_FTP_MAX_PATH_SIZE];
        strncpy(localPath, localDirectory, ARUTILS_FTP_MAX_PATH_SIZE);
        localPath[ARUTILS_FTP_MAX_PATH_SIZE - 1] = '\0';
        strncat(localPath, "/" ARDATATRANSFER_DATA_DOWNLOADER_FTP_ROOT, ARUTILS_FTP_MAX_PATH_SIZE - strlen(localPath) - 1);
        
        resultSys = mkdir(localPath, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
        
        if ((resultSys != 0) && (EEXIST != errno))
        {
            result = ARDATATRANSFER_ERROR_SYSTEM;
        }
    }
    
    if (result == ARDATATRANSFER_OK)
    {
        manager->dataDownloader->ftp = ARUTILS_Ftp_Connection_New(&manager->dataDownloader->threadSem, deviceIP, port, ARUTILS_FTP_ANONYMOUS, "", &error);
        
        if (error != ARUTILS_OK)
        {
            result = ARDATATRANSFER_ERROR_FTP;
        }
    }
        
    ARSAL_PRINT(ARSAL_PRINT_DEBUG, ARDATATRANSFER_DATA_DOWNLOADER_TAG, "return %d", result);
    
    return result;
}

void ARDATATRANSFER_DataDownloader_Clear(ARDATATRANSFER_Manager_t *manager)
{
    ARSAL_PRINT(ARSAL_PRINT_DEBUG, ARDATATRANSFER_DATA_DOWNLOADER_TAG, "");
    
    if (manager != NULL)
    {
        if (manager->dataDownloader->ftp != NULL)
        {
            ARUTILS_Ftp_Connection_Delete(&manager->dataDownloader->ftp);
        }
    }
}

int ARDATATRANSFER_DataDownloader_UsedMemoryCallback(const char* fpath, const struct stat *sb, int typeflag)
{
    if(typeflag == FTW_F)
    {
        dataFwt.sum += (double)sb->st_size;
    }

	return 0;
}

int ARDATATRANSFER_DataDownloader_RemoveDataCallback(const char* fpath, const struct stat *sb, int typeflag)
{
    if (strstr(fpath, ARDATATRANSFER_MANAGER_DOWNLOADER_PREFIX_DOWNLOADING) == NULL)
	{
		if(typeflag == FTW_F)
		{
            if (dataFwt.sum > dataFwt.allowedSpace)
            {
                ARUTILS_FileSystem_RemoveFile(fpath);
            }

			dataFwt.sum -= (double)sb->st_size;
        }
    }

    return 0;
}

eARDATATRANSFER_ERROR ARDATATRANSFER_DataDownloader_CheckUsedMemory(const char *localPath, float spacePercent)
{
    eARDATATRANSFER_ERROR result = ARDATATRANSFER_OK;
    eARUTILS_ERROR error = ARUTILS_OK;
    double availableSpace = 0.f;
    int resultSys = 0;

    ARSAL_PRINT(ARSAL_PRINT_DEBUG, ARDATATRANSFER_DATA_DOWNLOADER_TAG, "%s, %.f", localPath ? localPath : "null", spacePercent);

    error = ARUTILS_FileSystem_GetFreeSpace(localPath, &availableSpace);
    
    if (error != ARUTILS_OK)
    {
        result = ARDATATRANSFER_ERROR_SYSTEM;
    }

    if (result == ARDATATRANSFER_OK)
    {
        dataFwt.sum = 0;
        strncpy(dataFwt.dir, localPath, ARUTILS_FTP_MAX_PATH_SIZE);
        dataFwt.dir[ARUTILS_FTP_MAX_PATH_SIZE - 1] = '\0';

        resultSys = ARSAL_Ftw(dataFwt.dir, &ARDATATRANSFER_DataDownloader_UsedMemoryCallback, ARUTILS_FILE_SYSTEM_MAX_FD_FOR_FTW);

        if (resultSys != 0)
        {
            result = ARDATATRANSFER_ERROR_SYSTEM;
        }
        else
        {
            ARSAL_PRINT(ARSAL_PRINT_DEBUG, ARDATATRANSFER_DATA_DOWNLOADER_TAG, "sum %.f available %.f", (float)dataFwt.sum, (float)availableSpace);

            dataFwt.allowedSpace = (availableSpace * spacePercent) / 100.f;

            if (dataFwt.sum > dataFwt.allowedSpace)
            {
                resultSys = ARSAL_Ftw(dataFwt.dir, &ARDATATRANSFER_DataDownloader_RemoveDataCallback, ARUTILS_FILE_SYSTEM_MAX_FD_FOR_FTW);

                if (resultSys != 0)
                {
                    result = ARDATATRANSFER_ERROR_SYSTEM;
                }
            }
        }
    }

    return result;
}




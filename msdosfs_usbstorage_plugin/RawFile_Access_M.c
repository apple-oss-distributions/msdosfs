/* Copyright © 2017 Apple Inc. All rights reserved.
 *
 *  RawFile_Access_M.c
 *  msdosfs.kext
 *
 *  Created by Susanne Romano on 09/10/2017.
 */

#include "RawFile_Access_M.h"
#include "FileRecord_M.h"
#include "DirOPS_Handler.h"
#include "Logger.h"
#include "FAT_Access_M.h"

#define MAX_READ_WRITE_LENGTH (0x7ffff000)

size_t RAWFILE_read(NodeRecord_s* psFileNode, uint64_t uOffset, size_t uLength, void *pvBuf,int* piError)
{
    size_t uAcctuallyRead           = 0;
    FileSystemRecord_s* psFSRecord  = GET_FSRECORD(psFileNode);
    uint32_t uFileSize              = getuint32(psFileNode->sRecordData.sNDE.sDosDirEntry.deFileSize);
    uint32_t uClusterSize           = CLUSTER_SIZE(psFSRecord);
    uint32_t uSectorSize            = SECTOR_SIZE(psFSRecord);
    
    // Fill the buffer until the buffer is full or till the end of the file
    while ( uAcctuallyRead < uLength )
    {
        /* Clculate how many bytes are still missing to add to the buffer
         * If offset near end of file need to get only (uFileSize - uOffset)
         * else need to read as much as left (uLength - uAcctuallyRead)
         */
        uint64_t uBytesStillMissing = (uFileSize < (uOffset + uLength - uAcctuallyRead))? uFileSize - uOffset : uLength - uAcctuallyRead;
        uint64_t uBytesToCopy = 0;

        //No need to read anymore
        if (uBytesStillMissing == 0) {
            break;
        }

        // Get cluster number according to wanted offset
        uint32_t uCurrentCluster ,uContiguousClusterLength;
        FILERECORD_GetChainFromCache(psFileNode, uOffset, &uCurrentCluster, &uContiguousClusterLength, piError);
        if (*piError)
        {
            MSDOS_LOG(LEVEL_ERROR, "RAWFILE_read failed to get chain from cache = %d\n", *piError);
            return 0;
        }
        
        // If we reached to the end of the file stop reading
        if ((uContiguousClusterLength == 0) || (uOffset >= uFileSize))
        {
            break;
        }

        //Calculate offset - offset by sector and need to add the offset by sector
        uint64_t uReadOffset = DIROPS_VolumeOffsetForCluster( psFSRecord, uCurrentCluster ) + ( ROUND_DOWN(uOffset, uSectorSize)  % uClusterSize );

        // Check if uOffset isn't in metadata zone
        if (uReadOffset < psFSRecord->uMetadataZone) {
            MSDOS_LOG(LEVEL_ERROR, "RAWFILE_read read file offset is within metadata zone = %llu\n", uReadOffset);
            *piError = EFAULT;
            break;
        }

        // If offset not align to sector size, need to read only 1 cluster and memcpy it's end
        if ( (uOffset % uSectorSize) != 0 )
        {
            void* pvBuffer = malloc( uSectorSize );
            if (pvBuffer == NULL)
            {
                *piError = ENOMEM;
                return 0;
            }
            uint64_t uInSectorOffset = uOffset % uSectorSize;
            uBytesToCopy = MIN( uBytesStillMissing, uSectorSize - uInSectorOffset );

            // Read the content of the file
            if ( pread(psFSRecord->iFD, pvBuffer, uSectorSize, uReadOffset) != uSectorSize )
            {
                *piError = errno;
                MSDOS_LOG(LEVEL_ERROR, "RAWFILE_read failed to read err = %d\n", *piError);
                free(pvBuffer);
                return 0;
            }

            memcpy(pvBuf, pvBuffer + uInSectorOffset, uBytesToCopy);
            free(pvBuffer);
        }
        // If uBytesStillMissing < uClusterSize, need to read 1 sector and memcpy the begining
        else if ( uBytesStillMissing < uSectorSize )
        {
            void* pvBuffer = malloc( uSectorSize );
            if ( pvBuffer == NULL )
            {
                *piError = ENOMEM;
                return 0;
            }
            uBytesToCopy =  uBytesStillMissing;

            // Read the content of the file
            if ( pread( psFSRecord->iFD, pvBuffer, uSectorSize, uReadOffset ) != uSectorSize )
            {
                *piError = errno;
                MSDOS_LOG(LEVEL_ERROR, "RAWFILE_read failed to read err = %d\n", *piError);
                free(pvBuffer);
                return 0;
            }
            
            memcpy(pvBuf+uAcctuallyRead, pvBuffer, uBytesToCopy);
            free(pvBuffer);
        }
        // Can read buffer size chunk
        else
        {
            uBytesToCopy = (uint64_t)uContiguousClusterLength * (uint64_t)uClusterSize - ( uOffset % uClusterSize );
            uBytesToCopy = MIN( uBytesToCopy, MAX_READ_WRITE_LENGTH );
            
            if ( uBytesStillMissing < uBytesToCopy )
            {
                // Buffer size should be in sector granularity
                uBytesToCopy = ROUND_DOWN( uBytesStillMissing, uSectorSize );
            }

            if ( pread(psFSRecord->iFD, pvBuf+uAcctuallyRead, uBytesToCopy, uReadOffset) != uBytesToCopy )
            {
                *piError = errno;
                MSDOS_LOG(LEVEL_ERROR, "RAWFILE_read failed to read err = %d\n", *piError);
                return 0;
            }
        }

        // Update the amount of butes alreay read
        uAcctuallyRead += uBytesToCopy;
        // Update in file offset
        uOffset += uBytesToCopy;
    }
    
    return uAcctuallyRead;
}

static void registerUnAlignedCondTable(NodeRecord_s* psFileNode, uint64_t uOffset)
{

retry:
    pthread_mutex_lock(&psFileNode->sExtraData.sFileData.sUnAlignedWriteLck);
    uint64_t uSector;

    uSector = uOffset / SECTOR_SIZE(GET_FSRECORD(psFileNode));
    bool needToWait = false;
    pthread_cond_t* condToWait = NULL;
    int iFreeLocation = NUM_OF_COND;
    for (int iCond = 0; iCond < NUM_OF_COND; iCond++)
    {
        //Found someone that is writing into this sector
        if (psFileNode->sExtraData.sFileData.sCondTable[iCond].uSectorNum == uSector)
        {
            needToWait = true;
            condToWait = &psFileNode->sExtraData.sFileData.sCondTable[iCond].sCond;
            break;
        }
        
        if (psFileNode->sExtraData.sFileData.sCondTable[iCond].uSectorNum == 0 && iFreeLocation == NUM_OF_COND)
        {
            iFreeLocation = iCond;
        }
    }
    
    //If no one is locking us
    if (!needToWait)
    {
        //if we found a free location
        if (iFreeLocation != NUM_OF_COND)
        {
            psFileNode->sExtraData.sFileData.sCondTable[iFreeLocation].uSectorNum = uSector;
        } else {
            //we will wait on the last one to finish
            needToWait = true;
            condToWait = &psFileNode->sExtraData.sFileData.sCondTable[NUM_OF_COND - 1].sCond;
        }
    }
    
    pthread_mutex_unlock(&psFileNode->sExtraData.sFileData.sUnAlignedWriteLck);
    
    if (needToWait)
    {
        pthread_cond_wait(condToWait, &psFileNode->sExtraData.sFileData.sUnAlignedWriteLck);
        goto retry;
    }
}

static void unregisterUnAlignedCondTable(NodeRecord_s* psFileNode, uint64_t uOffset) {
 
    pthread_mutex_lock(&psFileNode->sExtraData.sFileData.sUnAlignedWriteLck);
    uint64_t uSector = uOffset / SECTOR_SIZE(GET_FSRECORD(psFileNode));
    bool unregistered = false;
    for (int iCond = 0; iCond < NUM_OF_COND; iCond++)
    {
        //Found my condition
        if (psFileNode->sExtraData.sFileData.sCondTable[iCond].uSectorNum == uSector)
        {
            psFileNode->sExtraData.sFileData.sCondTable[iCond].uSectorNum = 0;
            pthread_cond_broadcast(&psFileNode->sExtraData.sFileData.sCondTable[iCond].sCond);
            unregistered = true;
            break;
        }
    }
    assert(unregistered);
    pthread_mutex_unlock(&psFileNode->sExtraData.sFileData.sUnAlignedWriteLck);
}

size_t RAWFILE_write(NodeRecord_s* psFileNode, uint64_t uOffset, uint64_t uLength, void *pvBuf, int* piError)
{
    FileSystemRecord_s* psFSRecord  = GET_FSRECORD(psFileNode);
    size_t uAcctuallyWritten        = 0;
    uint32_t uClusterSize           = CLUSTER_SIZE(psFSRecord);
    uint32_t uSectorSize            = SECTOR_SIZE(psFSRecord);

    // Fill the buffer until the buffer is full or till the end of the file
    while ( uAcctuallyWritten < uLength )
    {
        /* Clculate how many bytes are still missing to add to the device
         * If offset near end of file need to set only (uFileSize - uOffset)
         * else need to write as much as left (uLength - uAcctuallyRead)
         */
        uint64_t uBytesStillMissing = uLength - uAcctuallyWritten;
        uint64_t uBytesToCopy = 0;

        // Get amount of contiguous clusters
        uint32_t uContiguousClusterLength, uCurrentCluster;
        FILERECORD_GetChainFromCache(psFileNode,(uint64_t) uOffset, &uCurrentCluster, &uContiguousClusterLength, piError);
        if (*piError) break;
        
        // If we reached to the end of the allocated clusters stop write
        if (uContiguousClusterLength == 0)
        {
            MSDOS_LOG(LEVEL_DEBUG, "RAWFILE_write can't write anymore data, we reached the end of the allocated space - %llu\n", uOffset);
            break;
        }

        //Calculate offset - offset by sector and need to add the offset by sector
        uint64_t uWriteOffset = DIROPS_VolumeOffsetForCluster( psFSRecord, uCurrentCluster ) + ( ROUND_DOWN(uOffset, uSectorSize)  % uClusterSize );

        // Check if uOffset isn't in metadata zone
        if (uWriteOffset < psFSRecord->uMetadataZone) {
            MSDOS_LOG(LEVEL_ERROR, "RAWFILE_write write file offset is within metadata zone = %llu\n", uWriteOffset);
            *piError = EFAULT;
            break;
        }

        // If offset not align to sector size, need to read the existing data
        // memcpy it's beginning and write back to the device
        if ( (uOffset % uSectorSize) != 0 )
        {
            void* pvBuffer = malloc( uSectorSize );
            if (pvBuffer == NULL)
            {
                *piError = ENOMEM;
                return 0;
            }

            registerUnAlignedCondTable(psFileNode, uWriteOffset);

            uint64_t uInSectorOffset = uOffset % uSectorSize;
            uBytesToCopy             = MIN( uBytesStillMissing, uSectorSize - uInSectorOffset );

            // Read the content of the existing file
            if ( pread( psFSRecord->iFD, pvBuffer, uSectorSize, uWriteOffset ) != uSectorSize )
            {
                *piError = errno;
                MSDOS_LOG(LEVEL_ERROR, "RAWFILE_write pread 1 failed with error = %d\n", *piError);
                free(pvBuffer);
                unregisterUnAlignedCondTable(psFileNode, uWriteOffset);
                return 0;
            }

            // memcpy the data from the given buffer
            memcpy(pvBuffer+uInSectorOffset, pvBuf, uBytesToCopy);

            //Write the data into the device
            if ( pwrite(psFSRecord->iFD, pvBuffer, uSectorSize, uWriteOffset) != uSectorSize )
            {
                *piError = errno;
                MSDOS_LOG(LEVEL_ERROR, "RAWFILE_write pwrite 1 failed with error = %d\n", *piError);
                free(pvBuffer);
                unregisterUnAlignedCondTable(psFileNode, uWriteOffset);
                return 0;
            }

            free(pvBuffer);
            unregisterUnAlignedCondTable(psFileNode, uWriteOffset);

        }
        // If uBytesStillMissing < uSectorSize, need to R/M/W 1 sector.
        else if ( uBytesStillMissing < uSectorSize )
        {
            void* pvBuffer = malloc( uSectorSize );
            if (pvBuffer == NULL)
            {
                *piError = ENOMEM;
                return 0;
            }

            registerUnAlignedCondTable(psFileNode, uWriteOffset);

            uBytesToCopy =  uBytesStillMissing;

            // Read the content of the existing file
            if ( pread(psFSRecord->iFD, pvBuffer, uSectorSize, uWriteOffset) != uSectorSize )
            {
                *piError = errno;
                MSDOS_LOG(LEVEL_ERROR, "RAWFILE_write pread 2 failed with error = %d\n", *piError);
                free(pvBuffer);
                unregisterUnAlignedCondTable(psFileNode, uWriteOffset);
                return 0;
            }

            //memcpy the last data
            memcpy(pvBuffer, pvBuf+uAcctuallyWritten, uBytesToCopy);

            // Read the content of the file
            if ( pwrite(psFSRecord->iFD, pvBuffer, uSectorSize, uWriteOffset) != uSectorSize )
            {
                *piError = errno;
                MSDOS_LOG(LEVEL_ERROR, "RAWFILE_write pwrite 2 failed with error = %d\n", *piError);
                free(pvBuffer);
                unregisterUnAlignedCondTable(psFileNode, uWriteOffset);
                return 0;
            }

            free(pvBuffer);
            unregisterUnAlignedCondTable(psFileNode, uWriteOffset);
        }
        // Can write buffer size chunk
        else
        {
            uBytesToCopy = (uint64_t)uContiguousClusterLength * (uint64_t)uClusterSize - ( uOffset % uClusterSize );
            uBytesToCopy = MIN( uBytesToCopy, MAX_READ_WRITE_LENGTH );
            if ( uBytesStillMissing < uBytesToCopy )
            {
                // Buffer size should be in sector granularity, need to round down
                uBytesToCopy = ROUND_DOWN( uBytesStillMissing, uSectorSize );
            }

            if ( pwrite(psFSRecord->iFD, pvBuf+uAcctuallyWritten, uBytesToCopy, uWriteOffset) != uBytesToCopy )
            {
                *piError = errno;
                MSDOS_LOG(LEVEL_ERROR, "RAWFILE_write pwrite 3 failed with error = %d\n", *piError);
                return 0;
            }
        }

        // Update the amount of butes alreay read
        uAcctuallyWritten += uBytesToCopy;
        // Update in file offset
        uOffset += uBytesToCopy;
    }

    return uAcctuallyWritten;
}

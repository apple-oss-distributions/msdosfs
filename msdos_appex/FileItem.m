/*
 * Copyright (c) 2023 Apple Inc. All rights reserved.
 */

#import <Foundation/Foundation.h>
#import "ExtensionCommon.h"
#import "FATManager.h"
#import "FATVolume.h"
#import "FATItem.h"
#import "utils.h"

@interface BlockmapRequest : NSObject

@property uint64_t originalFileSize;

@end

@implementation BlockmapRequest

-(instancetype)initWithOriginalSize:(uint64_t)originalSize
{
    self = [super init];
    if (self) {
        _originalFileSize = originalSize;
    }
    return self;
}

@end
@implementation FileItem

-(instancetype)initInVolume:(FATVolume *)volume
                      inDir:(FATItem * _Nullable)parentDir
                 startingAt:(uint32_t)firstCluster
                   withData:(DirEntryData * _Nullable)entryData
                    andName:(FSFileName *)name
{
    self = [super initInVolume:volume
                         inDir:parentDir
                    startingAt:firstCluster
                      withData:entryData
                       andName:name
                        isRoot:false];

    if (self) {
        self.isPreAllocated = false;
        self.blockmapRequests = [NSMutableDictionary dictionary];
        self.writeCounter = 0;
    }

    return self;
}

-(uint64_t)maxFileSize
{
    return 0; // sub-classes should implement.
}

-(NSError *)truncateTo:(uint64_t)newSize
          allowPartial:(bool)allowPartial
          mustBeContig:(bool)mustBeContig
{
    __block NSError *error = nil;
    uint64_t clusterSize = self.volume.systemInfo.bytesPerCluster;
    __block uint32_t newNumOfClusters = (uint32_t)(roundup(newSize, clusterSize) / clusterSize);

    /* Check that the new size is not too big */
    if (newSize > [self maxFileSize]) {
        return fs_errorForPOSIXError(EFBIG);
    }

    if (newNumOfClusters > self.numberOfClusters) {
        /* Need to allocate clusters */
        uint32_t numClustersToAlloc = newNumOfClusters - self.numberOfClusters;
        [self.volume.fatManager allocateClusters:numClustersToAlloc
                                         forItem:self
                                    allowPartial:allowPartial
                                    mustBeContig:mustBeContig
                                        zeroFill:false
                                    replyHandler:^(NSError * _Nullable allocError,
                                                   uint32_t firstAllocatedCluster,
                                                   uint32_t lastAllocatedCluster,
                                                   uint32_t numAllocated) {
            if (allocError) {
                os_log_error(OS_LOG_DEFAULT, "%s: Failed to allocate clusters. Error = %@.", __func__, allocError);
                error = allocError;
            }
        }];
    } else if (newNumOfClusters < self.numberOfClusters) {
        /* Need to free clusters */
        uint32_t numClustersToFree = self.numberOfClusters - newNumOfClusters;
        [self.volume.fatManager freeClusters:numClustersToFree
                                      ofItem:self
                                replyHandler:^(NSError *freeError) {
            if (freeError) {
                os_log_error(OS_LOG_DEFAULT, "%s: Failed to free clusters. Error = %@.", __func__, freeError);
                error = freeError;
            }
        }];
    }
    return error;
}

-(void)blockmapOffset:(off_t)offset
               length:(size_t)length
               flags:(FSBlockmapFlags)flags
         operationID:(FSOperationID)operationID
              packer:(FSExtentPacker *)packer
        replyHandler:(nonnull void (^)(NSError * _Nullable error))reply
{
    __block NSError *error = nil;
    uint64_t clusterSize = self.volume.systemInfo.bytesPerCluster;


    os_log_debug(OS_LOG_DEFAULT, "%s: offset: %llu, length: %zu, flags: %lu, operationID: %lu.\n", __func__, offset, length, flags, operationID);

    /* Check that we got either FSKIT_FLAGS_WRITE or FSKIT_FLAGS_READ in flags, or we're in no-IO case. */
    bool isWrite;
    if (flags & FSBlockmapFlagsWrite) {
        isWrite = true;
    } else if ((flags & FSBlockmapFlagsRead) || operationID == FSOperationIDUnspecified) {
        /* In case operationID == FSOperationIDUnspecified, no IO will be performed by the kernel.
          We treat this case as read, as we don't expect an corresponding endIO call. */
        isWrite = false;
    } else {
        os_log_error(OS_LOG_DEFAULT, "%s: Neither FSKIT_FLAGS_WRITE nor FSKIT_FLAGS_READ bit is set in flags.\n", __func__);
        return reply(fs_errorForPOSIXError(EINVAL));
    }

    if (length == 0) {
        os_log(OS_LOG_DEFAULT, "%s: Requested length = 0. Exit with SUCCESS and numOfExtentsFetched = 0.", __func__);
        return reply(nil);
    }

    if (offset + length > [self maxFileSize]) {
        os_log_error(OS_LOG_DEFAULT, "%s: Given length + offset is too big.\n", __func__);
        return reply(fs_errorForPOSIXError(EFBIG));
    }

    /* For writes - Set the dirty bit. */
    if (isWrite) {
        [self.volume.fatManager setDirtyBitValue:dirtyBitDirty
                                    replyHandler:^(NSError * _Nullable fatError) {
            if (fatError) {
                /* just log the error, don't fail the blockmap. */
                os_log_error(OS_LOG_DEFAULT, "%s: Couldn't set the dirty bit. Error = %@. \n", __func__, fatError);
            }
        }];
    }

    uint64_t originalAllocatedSize  = self.numberOfClusters * clusterSize;
    uint64_t originalSize           = [self.entryData getSize];
    uint64_t effectiveLength        = length;
    uint64_t writeEndOffset         = offset + length;
    uint64_t newAllocatedSize       = originalAllocatedSize;
    uint64_t newSize                = writeEndOffset;

    if (!isWrite && offset >= originalAllocatedSize) {
        os_log_error(OS_LOG_DEFAULT, "%s: Read with requested offset (%llu) >= file allocated size (%llu). Exiting.", __func__, offset, originalAllocatedSize);
        return reply(fs_errorForPOSIXError(EINVAL));
    }

    if (isWrite) {
        if (originalAllocatedSize < originalSize) {
            /* The file is probably corrupted in such case.
             Therefore we don't allow writing to this file. */
            os_log_error(OS_LOG_DEFAULT, "%s: allocated size (%llu) < file size (%llu). The file is probably corrupted. Exiting.",
                      __func__, originalAllocatedSize, originalSize);
            return reply(fs_errorForPOSIXError(EIO));
        }

        if (writeEndOffset > originalSize) {
            if (writeEndOffset > originalAllocatedSize) {
                /* We need to allocate more clusters. */
                error = [self truncateTo:writeEndOffset
                            allowPartial:true
                            mustBeContig:false];
                if (error) {
                    os_log_error(OS_LOG_DEFAULT, "%s: Couldn't truncate file. Error = %@.", __func__, error);
                    return reply(error);
                }
                newAllocatedSize = self.numberOfClusters * clusterSize;
                if (newAllocatedSize <= offset) {
                    /* We couldn't allocate enough space to even get to the offset.
                     Free the newly allocated clusters and exit. */
                    os_log_error(OS_LOG_DEFAULT, "%s: Failed to allocate enough clusters for wanted offset and length.", __func__);
                    [self truncateTo:originalSize
                        allowPartial:false
                        mustBeContig:false];
                    return reply(fs_errorForPOSIXError(ENOSPC));
                } else if (newAllocatedSize < writeEndOffset) {
                    /* Couldn't allocate as much as needed, adjusting the length to write. */
                    effectiveLength = newAllocatedSize - offset;
                    newSize = newAllocatedSize;
                    os_log_debug(OS_LOG_DEFAULT, "%s: Couldn't allocate all clusters for wanted offset and length. Length to write = %llu (instead of %zu).\n", __func__, effectiveLength, length);
                }
            }
            /* Update dir entry with new size. Don't flush it until we know that the IO succeeded (in endIO). */
            [self.entryData setSize:newSize];
        }
    }

    /* Do the actual blockmap work */
    [self fetchFileExtentsFrom:offset
                            to:newAllocatedSize
                   usingBlocks:packer
                  replyHandler:^(NSError *fetchError) {
        if (fetchError) {
            os_log_error(OS_LOG_DEFAULT, "%s: Couldn't fetch file extents. Error = %@", __func__, fetchError);
            error = fetchError;
        }
    }];

    if (error == nil) {
        if (isWrite &&
            (operationID != FSOperationIDUnspecified) &&
            self.blockmapRequests[[NSNumber numberWithUnsignedLong:operationID]] == nil) {
             /* Add the request to the list only for the first blockmap call of a write operation.
              * (Currently MSDOS/ExFAT don't support access time updates (mounted by default with MNT_NOATIME),
              * so there's no need to call completeIO for read operations.
              * Therefore we don't need to maintain any state for such requests). */
            [self.blockmapRequests setObject:[[BlockmapRequest alloc] initWithOriginalSize:originalSize]
                                      forKey:[NSNumber numberWithUnsignedLong:operationID]];
            /* Increment the write counter for the first blockmap call of a write operation
             (if we got here, we know that the operation can't fail). */
            self.writeCounter++;
        }
    } else {
        /* In case of an error, free the newly allocated clusters and revert the file size change. */
        [self truncateTo:originalSize
            allowPartial:false
            mustBeContig:false];
        [self.entryData setSize:originalSize];
        return reply(error);
    }

    return reply(nil);
}

-(NSError *)completeIOAtOffset:(off_t)offset
                        length:(size_t)length
                       status:(int)ioStatus
                        flags:(FSCompleteIOFlags)flags
                  operationID:(FSOperationID)operationID
{
    uint64_t clusterSize = self.volume.systemInfo.bytesPerCluster;
    uint64_t endOffset = (uint64_t)offset + length;
    BlockmapRequest *blockmapRequest = nil;
    __block bool shouldFlushEntryData = false;
    NSError *error = nil;

    os_log_debug(OS_LOG_DEFAULT, "%s: offset: %lu, length: %lu, status: %d, flags: %lu, operationID: %lu.",
                 __func__, (unsigned long)offset, (unsigned long)length, ioStatus, flags, operationID);

    if (flags & FSBlockmapFlagsRead) {
        /*
         * Currently MSDOS doesn't support access time updates (mounted by default with MNT_NOATIME),
         * so there's no need to call EndBlockmap for read operations.
         * Therefore it's not allowed.
         */
        os_log_error(OS_LOG_DEFAULT, "%s: No endIO calls should be made for reads.", __func__);
        return fs_errorForPOSIXError(EINVAL);
    }

    /*
     * Reduce the write counter if needed.
     * In case of there was no corresponding blockmapFile call,
     * we didn't increment the counter ('overwrite' case).
     * That's ok becuase the upper layers would make sure no reclaims would be made
     * as long as we have ongoing writes.
     */
    if (operationID != FSOperationIDUnspecified) {
        self.writeCounter--;
        blockmapRequest = [self.blockmapRequests objectForKey:[NSNumber numberWithUnsignedLong:operationID]];
        if (!blockmapRequest) {
            os_log_fault(OS_LOG_DEFAULT, "%s: Couldn't find blockmap request (%lu) in dictionary.", __func__, operationID);
            return fs_errorForPOSIXError(EINVAL);
        }
    }

    /* Set the dirty bit, as we're going to flush the dir entry to disk. */
    [self.volume.fatManager setDirtyBitValue:dirtyBitDirty
                                replyHandler:^(NSError * _Nullable fatError) {
        if (fatError) {
            /* just log the error, don't fail the endIO. */
            os_log_error(OS_LOG_DEFAULT, "%s: Couldn't set the dirty bit. Error = %@. \n", __func__, fatError);
        }
    }];

    if (ioStatus == 0) {
        /* NOTE: we assume here that all of the endIO calls are made for write operations.
         (in case of a read we shouldn't reach here because we return an error above). */

        uint64_t allocatedSize = self.numberOfClusters * clusterSize;
        if (endOffset > allocatedSize) {
            /* To write beyond the file allocated size, beginBlockmap must be called first. */
            os_log_error(OS_LOG_DEFAULT, "%s: offset + length > file's allocated size (%llu > %llu).", __func__, endOffset, allocatedSize);
            return fs_errorForPOSIXError(EINVAL);
        }

        /* In case we've enlarged the file size in blockmapFile,
         * we should flush the dir entry to disk. */
        if (blockmapRequest && (endOffset > blockmapRequest.originalFileSize)) {
            shouldFlushEntryData = true;
        }

        /* Update the file size if needed.
         * (There could be cases in which the endIO's length > blockmapFile's length
         *  for the same request, because of extent caching done by lifs kext). */
        if (endOffset > [self.entryData getSize]) {
            [self.entryData setSize:endOffset];
            shouldFlushEntryData = true;
        }

        // Update the valid data length if needed
        if (endOffset > [self.entryData getValidDataLength]) {
            [self.entryData setValidDataLength:endOffset];
            shouldFlushEntryData = true;
        }

        /* Update modification time */
        [self updateModificationTime:^(bool mTimeWasUpdated) {
            if (mTimeWasUpdated) {
                shouldFlushEntryData = true;
            }
        }];

        /* Update preallocation status (in case we've used all pre-allocated clusters). */
        [self updatePreallocStatus];

        /* Flush entry data if needed (and the item is not open-unlinked). */
        if (shouldFlushEntryData && (self.isDeleted == false)) {
            error = [self flushDirEntryData];
            if (error) {
                os_log_error(OS_LOG_DEFAULT, "%s: Failed to update dir entry with new size. Error = %@.", __func__, error);
            }
        }
    }

    if (error || ioStatus != 0) {
        /*
         * Revert the file size only if:
         * 1. This is not an 'overwrite' case (blockmapRequest != nil)
         * AND
         * 2. We enlarged the file in the corresponding blockmapFile call.
         * AND
         * 3. There are no other blockmaps in parallel.
         */
        if (blockmapRequest && endOffset > blockmapRequest.originalFileSize && self.writeCounter == 0) {
            /* We shouldn't flush the entry to disk, as the on-disk entry still contains the original size. */
            [self truncateTo:blockmapRequest.originalFileSize
                allowPartial:false
                mustBeContig:false];
            [self.entryData setSize:blockmapRequest.originalFileSize];
        }
    }

    /* Remove the blockmap request from list */
    if (blockmapRequest) {
        [self.blockmapRequests removeObjectForKey:[NSNumber numberWithUnsignedLong:operationID]];
    }
    return error;
}

-(void)fetchFileExtentsFrom:(uint64_t)startOffset
                         to:(uint64_t)endOffset
                usingBlocks:(FSExtentPacker *)packer
               replyHandler:(nonnull void (^)(NSError * _Nullable error))reply
{
    __block NSError *error = nil;
    __block uint32_t curNextCluster = 0;
    __block uint32_t numContigClusters = 0;
    uint64_t clusterSize = self.volume.systemInfo.bytesPerCluster;
    uint64_t sectorSize = self.volume.systemInfo.bytesPerSector;
    uint64_t startClusterIdx = startOffset / clusterSize;
    uint64_t clusterIdx = (self.firstClusterIndexInLastAllocation <= startClusterIdx) ? self.firstClusterIndexInLastAllocation : 0;
    
    uint32_t curStartCluster;
    if (clusterIdx) {
        curStartCluster = self.firstClusterInLastAllocation;
    } else {
        curStartCluster = self.firstCluster;
    }
    uint64_t currentOffsetInFile = startOffset;

    bool startIterating = false;

    uint32_t extentIndex = 0;
    uint64_t extentOffset = 0;
    uint64_t extentLength = 0;
    FSExtentType extentType = 0;

    while ([self.volume.systemInfo isClusterValid:curStartCluster]) {
        [self.volume.fatManager getContigClusterChainLengthStartingAt:curStartCluster
                                                         replyHandler:^(NSError * _Nullable fatError,
                                                                        uint32_t length,
                                                                        uint32_t nextClusterInChain) {
            if (fatError) {
                error = fatError;
            } else {
                numContigClusters = length;
                curNextCluster = nextClusterInChain;
            }
        }];

        if (error) {
            os_log_error(OS_LOG_DEFAULT, "%s: Failed to get the next cluster(s). Error = %@.", __func__, error);
            break;
        }

        if (!startIterating) {
            if ((clusterIdx <= startClusterIdx) &&
                (clusterIdx + numContigClusters > startClusterIdx)) {
                /* We've got to a cluster we're interested in.
                 adjust startCluster and start iterating */
                int clusterShift = (int)(startClusterIdx - clusterIdx);
                curStartCluster += clusterShift;
                numContigClusters -= clusterShift;
                startIterating = true;
            } else {
                clusterIdx += numContigClusters;
                curStartCluster = curNextCluster;
                continue;
            }
        }

        extentOffset = [self.volume.systemInfo offsetForCluster:curStartCluster] + (currentOffsetInFile % clusterSize);

        /* Check if volumeOffset isn't in metadata zone */
        if ([self.volume isOffsetInMetadataZone:extentOffset]) {
            os_log_error(OS_LOG_DEFAULT, "%s: file offset is within metadata zone = %llu.\n", __func__, extentOffset);
            error = fs_errorForPOSIXError(EFAULT);
            break;
        }

        /* Calculate the extent's length */
        extentLength = numContigClusters * clusterSize - (currentOffsetInFile % clusterSize);
        /* Make sure the returned extent length is sector-aligned. */
        extentLength = roundup(extentLength, sectorSize);
        /* Clip the extent length to UINT32_MAX, while keeping it sector-aligned. */
        extentLength = MIN(extentLength, ROUND_DOWN(UINT32_MAX, sectorSize));

        /*
         * All extents are DATAFILL extents for file systems which don't support
         * sparse files.  The kernel knows that it should return zeros for reads
         * beyond EOF.
         */
        extentType = FSExtentTypeData;

        if (![packer packExtentWithResource:self.volume.resource
                                       type:extentType
                              logicalOffset:currentOffsetInFile
                             physicalOffset:extentOffset
                                     length:extentLength]) {
            break;
        }

        uint64_t extentSizeInCluster = (extentLength + (currentOffsetInFile % clusterSize)) / clusterSize;
        if (numContigClusters > extentSizeInCluster) {
            curStartCluster += extentSizeInCluster;
        } else {
            curStartCluster = curNextCluster;
        }

        currentOffsetInFile += extentLength;
    }

    if (error) {
        /* If we already fetched some extents, we return them without an error. */
        if (extentIndex == 0) {
            return reply(error);
        }
    }

    return reply(nil);
}

-(void)updateModificationTime:(nonnull void (^)(bool mTimeWasUpdated))reply
{
    struct timespec fileMTime = {0};
    struct timespec currentTime = {0};
    CONV_GetCurrentTime(&currentTime);
    [self.entryData getModifyTime:&fileMTime];

    if (fileMTime.tv_sec != currentTime.tv_sec) {
        /* Update only if there is a diff in the modified time. */
        [self.entryData setModifyTime:&currentTime];
        return reply(true);
    }
    return reply(false);
}

-(void)updatePreallocStatus
{
    uint64_t clusterSize = self.volume.systemInfo.bytesPerCluster;
    uint32_t numberOfUsedClusters = (uint32_t)(roundup([self.entryData getSize], clusterSize) / clusterSize);

    if (numberOfUsedClusters == self.numberOfClusters) {
        [self setPreAllocated:false];
    } else {
        [self setPreAllocated:true];
    }
}

-(void)setPreAllocated:(bool)isPreallocated
{
    if (isPreallocated != self.isPreAllocated) {
        /* Consider multiple FDs for the same file */
        if (isPreallocated) {
            [self.volume incNumberOfPreallocatedFiles];
        } else {
            if ([self.volume getNumberOfPreallocatedFiles] == 0) {
                os_log_error(OS_LOG_DEFAULT, "%s: Expected number of preallocated files to be > 0", __FUNCTION__);
            } else {
                [self.volume decNumberOfPreallocatedFiles];
            }
        }
        self.isPreAllocated = isPreallocated;
    }
}

-(void)preallocate:(uint64_t)size
      allowPartial:(bool)allowPartial
      mustBeContig:(bool)mustBeContig
      replyHandler:(nonnull void (^)(NSError * _Nullable error,
                                     uint64_t allocatedSize))reply
{
    uint64_t clusterSize = self.volume.systemInfo.bytesPerCluster;
    uint64_t curAllocatedSize = self.numberOfClusters * clusterSize;
    uint64_t desiredFileSize = size + curAllocatedSize;
    uint32_t desiredFileSizeInClusters = (uint32_t)(ROUND_UP(desiredFileSize, clusterSize) / clusterSize);
    uint32_t clustersToAlloc = desiredFileSizeInClusters - self.numberOfClusters;
    NSError *err = nil;

    /* Check that the new file size is not too big. */
    if (desiredFileSize > DOS_FILESIZE_MAX) {
        return reply(fs_errorForPOSIXError(EFBIG), 0);
    }

    /* If we already have enough allocated clusters, do nothing. */
    if (clustersToAlloc == 0) {
        return reply(nil, 0);
    }

    /*
     * In case there's not enough space for desiredFileSize:
     * - if allowPartial is true, allocateClusters will allocate the
     * available clusters and return no error.  self.numberOfClusters is
     * updated with the amount of allocated clusters.
     * - else, allocateClusters will return ENOSPC without allocating at all.
     */
    err = [self truncateTo:desiredFileSize
              allowPartial:allowPartial
              mustBeContig:mustBeContig];

    if (err == nil) {
        [self updatePreallocStatus];
    }

    return reply(err, ((self.numberOfClusters * clusterSize) - curAllocatedSize));
}

/*
 * MSDOS always supports KOIO, make sure the inhibitKernelOffloadedIO property is set to false.
 */
-(FSItemAttributes *)getAttributes:(nonnull FSItemGetAttributesRequest *)desired
{
    FSItemAttributes *attrs = [super getAttributes:desired];
    return attrs;
}

@end

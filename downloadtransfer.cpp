#include "downloadtransfer.h"
#include <QThread>

DownloadTransfer::DownloadTransfer(QObject *parent) : Transfer(parent)
{
    //TODO: load transferSegmentStateBitmap from db
    downloadBucketTable = new QHash<int, QByteArray*>;
    transferRate = 0;
    transferProgress = 0;
    bytesWrittenSinceUpdate = 0;
    totalBucketsFlushed = 0;
    initializationStateTimerBrakes = 0;
    status = TRANSFER_STATE_INITIALIZING;
    remoteHost = QHostAddress("0.0.0.0");
    currentActiveSegments = 0;

    transferRateCalculationTimer = new QTimer();
    connect(transferRateCalculationTimer, SIGNAL(timeout()), this, SLOT(transferRateCalculation()));
    transferRateCalculationTimer->setSingleShot(false);
    transferRateCalculationTimer->start(1000);

    transferTimer = new QTimer();
    connect(transferTimer, SIGNAL(timeout()), this, SLOT(transferTimerEvent()));
    transferTimer->setSingleShot(false);
}

DownloadTransfer::~DownloadTransfer()
{
    //TODO: save transferSegmentStateBitmap to db
    transferRateCalculationTimer->deleteLater();
    transferTimer->deleteLater();
    QHashIterator<int, QByteArray*> itdb(*downloadBucketTable);
    while (itdb.hasNext())
    {
        // TODO: save halwe buckets na files toe
        delete itdb.next().value();
    }
    delete downloadBucketTable;

    //Sover ek verstaan gaan downloadBucketHashLookupTable al uit scope uit voor jy by hierdie destructor kom?
    //So jy moet of hom 'n pointer maak of net hierdie stap heeltemal uithaal
    //A: DownloadTransfer class variable, hy behoort nog hier te wees. ons wil die qbytearray pointers binne-in die ding delete.
    QHashIterator<int, QByteArray*> ithb(downloadBucketHashLookupTable);
    while (ithb.hasNext())
        delete ithb.next().value();

    QHashIterator<QHostAddress, RemotePeerInfoStruct> r(remotePeerInfoTable);
    while (r.hasNext())
        delete r.value().transferSegment;
}

void DownloadTransfer::incomingDataPacket(quint8, quint64 offset, QByteArray data)
{
    // map sorted from big to small
    // QMap::lowerBound will most likely find the segment just before the one we are interested in
    QMap<quint64, TransferSegmentTableStruct>::const_iterator i = transferSegmentTable.lowerBound(offset);
    if (Q_UNLIKELY(i == transferSegmentTable.constEnd()))
        return;

    if (Q_UNLIKELY(i.key() <= offset && i.value().segmentEnd >= offset))
        i.value().transferSegment->incomingDataPacket(offset, data);
    else if (Q_LIKELY(++i != transferSegmentTable.constEnd()))
    {
        if (Q_LIKELY(i.key() <= offset && i.value().segmentEnd >= offset))
            i.value().transferSegment->incomingDataPacket(offset, data);
    }
    // if the offset was not found like this, it probably is not in the map.

    /*while (i.hasPrevious())
    {
        i.previous();
        if (i.key() <= offset && i.value().segmentEnd >= offset)
        {

            break;
        }

    }*/
    bytesWrittenSinceUpdate += data.size();
}

void DownloadTransfer::hashBucketReply(int bucketNumber, QByteArray bucketTTH)
{
    if (downloadBucketHashLookupTable.contains(bucketNumber))
    {
        if (*downloadBucketHashLookupTable.value(bucketNumber) == bucketTTH)
            flushBucketToDisk(bucketNumber);
        else
        {
            // TODO: emit MISTAKE!
        }
    }
    // TODO: must check that tth tree item was received before requesting bucket hash.
}

void DownloadTransfer::TTHTreeReply(QByteArray tree)
{
    while (tree.length() >= 30)
    {
        int bucketNumber = getQuint32FromByteArray(&tree);
        int tthLength = getQuint16FromByteArray(&tree);
        QByteArray *bucketHash = new QByteArray(tree.left(tthLength));
        tree.remove(0, tthLength);
        downloadBucketHashLookupTable.insert(bucketNumber, bucketHash);
    }
}

int DownloadTransfer::getTransferType()
{
    return TRANSFER_TYPE_DOWNLOAD;
}

void DownloadTransfer::startTransfer()
{
    //QThread *thisThread = QThread::currentThread();
    //QThread *thatThread = transferTimer->thread();
    lastBucketNumber = calculateBucketNumber(fileSize);
    lastBucketSize = fileSize % HASH_BUCKET_SIZE;
    if (transferSegmentStateBitmap.length() == 0)
        for (int i = 0; i <= lastBucketNumber; i++)
            transferSegmentStateBitmap.append(SegmentNotDownloaded);

    transferTimer->start(100);
}

void DownloadTransfer::pauseTransfer()
{
    status = TRANSFER_STATE_PAUSED;  //TODO
}

void DownloadTransfer::abortTransfer()
{
    status = TRANSFER_STATE_ABORTING;
    emit abort(this);
}

void DownloadTransfer::addPeer(QHostAddress peer)
{
    if (peer.toIPv4Address() > 0 && !listOfPeers.contains(peer))
    {
        listOfPeers.append(peer);  // TODO: is this list then strictly necessary?
        emit requestProtocolCapability(peer);
    }
}

// currently copied from uploadtransfer.
// we need the freedom to change this if necessary, therefore, it is not implemented in parent class.
void DownloadTransfer::transferRateCalculation()
{
    if ((status == TRANSFER_STATE_RUNNING) && (bytesWrittenSinceUpdate == 0))
        status = TRANSFER_STATE_STALLED;
    else if ((status == TRANSFER_STATE_STALLED) && (bytesWrittenSinceUpdate > 0))
        status = TRANSFER_STATE_RUNNING;

    // snapshot the transfer rate as the amount of bytes written in the last second
    transferRate = bytesWrittenSinceUpdate;
    bytesWrittenSinceUpdate = 0;

    transferProgress = (int)(((double)totalBucketsFlushed / (calculateBucketNumber(fileSize) + 1)) * 100);
}

void DownloadTransfer::flushBucketToDisk(int &bucketNumber)
{
    // TODO: decide where to store these files
    QString tempFileName;
    tempFileName.append(TTHBase32);
    tempFileName.append(".");
    tempFileName.append(QString::number(bucketNumber));

    emit flushBucket(tempFileName, downloadBucketTable->value(bucketNumber));
    emit assembleOutputFile(TTHBase32, filePathName, bucketNumber, lastBucketNumber);

    // just remove entry, bucket pointer gets deleted in BucketFlushThread
    downloadBucketTable->remove(bucketNumber);
    transferSegmentStateBitmap[bucketNumber] = SegmentDownloaded;
    totalBucketsFlushed++;
    if (totalBucketsFlushed == calculateBucketNumber(fileSize) + 1)
    {
        status = TRANSFER_STATE_FINISHED;
        emit transferFinished(TTH);
    }
}

void DownloadTransfer::transferTimerEvent()
{
    if (status == TRANSFER_STATE_INITIALIZING)
    {
        // this is really primitive
        initializationStateTimerBrakes++;
        if (initializationStateTimerBrakes > 1)
        {
            if (initializationStateTimerBrakes == 100) // 10s with 100ms timer period
                initializationStateTimerBrakes = 0;
            return;
        }

        // Get peers and TTH tree
        if (listOfPeers.isEmpty())
        {
            //At this stage this will never be called since addPeer() is called as this object is created
            emit searchTTHAlternateSources(TTH);
        }
        else if (!(downloadBucketHashLookupTable.size() - 1 == lastBucketNumber))
        {
            // simple and stupid for now...
            qDebug() << "DownloadTransfer::transferTimerEvent(): " << listOfPeers;
            emit TTHTreeRequest(listOfPeers.first(), TTH);
        }
        else
        {
            status = TRANSFER_STATE_RUNNING;

        }
    }
}

/*void DownloadTransfer::setProtocolPreference(QByteArray &preference)
{
    protocolPreference = preference;
}*/

inline int DownloadTransfer::calculateBucketNumber(quint64 fileOffset)
{
    return (int)fileOffset >> 20;
}

void DownloadTransfer::segmentCompleted(TransferSegment *segment)
{
    qint64 lastTransferTime = QDateTime::currentMSecsSinceEpoch() - segment->getSegmentStartTime();
    int lastSegmentLength = (segment->getSegmentEnd() - segment->getSegmentStart()) / HASH_BUCKET_SIZE;
    int nextSegmentLengthHint = (10000 / lastTransferTime) * lastSegmentLength;
    if (nextSegmentLengthHint == 0)
        nextSegmentLengthHint = 1;

    transferSegmentTable.remove(segment->getSegmentStart());
    downloadNextAvailableChunk(segment, nextSegmentLengthHint);
}

void DownloadTransfer::segmentFailed(TransferSegment *segment)
{
    // remote end dead, segment given up hope. mark everything not downloaded as not downloaded, so that
    // the block allocator can give them to other segments that do work.
    // do not worry if this is the last working segment, if it breaks, it can resume on successful TTH search reply.
    int startBucket = calculateBucketNumber(segment->getSegmentStart());
    int endBucket = calculateBucketNumber(segment->getSegmentEnd());
    for (int i = startBucket; i <= endBucket; i++)
        if (transferSegmentStateBitmap.at(i) == SegmentCurrentlyDownloading)
            transferSegmentStateBitmap[i] = SegmentNotDownloaded;

    transferSegmentTable.remove(segment->getSegmentStart());
    currentActiveSegments--;
    // currently the object keeps sitting in remotePeerInfoTable and gets destroyed once the download completes.
    // this can be improved (TODO)
}

SegmentOffsetLengthStruct DownloadTransfer::getSegmentForDownloading(int segmentNumberOfBucketsHint)
{
     // Ideas for quick and dirty block allocator:
    // Scan once over bitmap, taking note of starting point and length of longest open segment
    // Allocate block immediately if long enough gap found, otherwise allocate longest possible gap
    SegmentOffsetLengthStruct segment;
    int longestSegmentStart = 0;
    int longestSegmentLength = 0;
    int currentSegmentStart = 0;
    int currentSegmentLength = 0;
    char lastSegmentState = SegmentDownloaded;
    for (int i = 0; i < transferSegmentStateBitmap.length(); i++)
    {
        if (transferSegmentStateBitmap.at(i) == SegmentNotDownloaded)
        {
            if (lastSegmentState != SegmentNotDownloaded)
            {
                currentSegmentStart = i;
                currentSegmentLength = 1;
            }
            else
            {
                currentSegmentLength++;
            }
            if (currentSegmentLength == segmentNumberOfBucketsHint)
            {
                segment.segmentBucketOffset = currentSegmentStart;
                segment.segmentBucketCount = currentSegmentLength;
                for (int j = currentSegmentStart; j < currentSegmentStart+currentSegmentLength; j++)
                    transferSegmentStateBitmap[j] = SegmentCurrentlyDownloading;
                return segment;
            }
            if (currentSegmentLength > longestSegmentLength)
            {
                longestSegmentStart = currentSegmentStart;
                longestSegmentLength = currentSegmentLength;
            }
        }
        lastSegmentState = transferSegmentStateBitmap.at(i);
    }
    segment.segmentBucketOffset = longestSegmentStart;
    segment.segmentBucketCount = longestSegmentLength;
    for (int j = longestSegmentStart; j < longestSegmentStart+longestSegmentLength; j++)
        transferSegmentStateBitmap[j] = SegmentCurrentlyDownloading;
    return segment;
}

void DownloadTransfer::receivedPeerProtocolCapability(QHostAddress peer, quint8 protocols)
{
    newPeer(peer, protocols);
    if (currentActiveSegments < MAXIMUM_SIMULTANEOUS_SEGMENTS)
    {
        currentActiveSegments++;
        TransferSegment *download = createTransferSegment(peer);
        if (download)
            downloadNextAvailableChunk(download);
    }
}

void DownloadTransfer::newPeer(QHostAddress peer, quint8 protocols)
{
    if (!remotePeerInfoTable.contains(peer))
    {
        RemotePeerInfoStruct rpis;
        rpis.bytesTransferred = 0;
        rpis.protocolCapability = protocols;
        rpis.transferSegment = 0;
        remotePeerInfoTable.insert(peer, rpis);
    }
}

TransferSegment* DownloadTransfer::createTransferSegment(QHostAddress peer)
{
    TransferSegment *download = 0;
    for (int i = 0; i < protocolOrderPreference.length(); i++)
    {
        if (remotePeerInfoTable.value(peer).triedProtocols.contains(protocolOrderPreference.at(i)))
            continue;
        if (protocolOrderPreference.at(i) & remotePeerInfoTable.value(peer).protocolCapability)
        {
            TransferProtocol p = (TransferProtocol)protocolOrderPreference.at(i);
            remotePeerInfoTable[peer].triedProtocols.append(p);
            download  = newConnectedTransferSegment(p);
            remotePeerInfoTable[peer].transferSegment = download;
            download->setDownloadBucketTablePointer(downloadBucketTable);
            download->setRemoteHost(peer);
            download->setTTH(TTH);
            download->setFileSize(fileSize);
            break;
        }
    }
    return download;
}

TransferSegment* DownloadTransfer::newConnectedTransferSegment(TransferProtocol p)
{
    TransferSegment* download = 0;
    switch(p)
    {
    case FailsafeTransferProtocol:
        download = new FSTPTransferSegment;
        break;
    case BasicTransferProtocol:
    case uTPProtocol:
    case ArpmanetFECProtocol:
        break;
    }
    connect(download, SIGNAL(hashBucketRequest(QByteArray,int,QByteArray*)), this, SIGNAL(hashBucketRequest(QByteArray,int,QByteArray*)));
    connect(download, SIGNAL(sendDownloadRequest(quint8,QHostAddress,QByteArray,quint64,quint64)),
            this, SIGNAL(sendDownloadRequest(quint8,QHostAddress,QByteArray,quint64,quint64)));
    connect(download, SIGNAL(transmitDatagram(QHostAddress,QByteArray*)), this, SIGNAL(transmitDatagram(QHostAddress,QByteArray*)));
    connect(transferTimer, SIGNAL(timeout()), download, SLOT(transferTimerEvent()));
    connect(download, SIGNAL(requestNextSegment(TransferSegment*)), this, SLOT(segmentCompleted(TransferSegment*)));
    connect(download, SIGNAL(transferRequestFailed(TransferSegment*)), this, SLOT(segmentFailed(TransferSegment*)));
    connect(download, SIGNAL(requestPeerProtocolCapability(QHostAddress)), this, SIGNAL(requestProtocolCapability(QHostAddress)));
    return download;
}

void DownloadTransfer::downloadNextAvailableChunk(TransferSegment *download, int length)
{
    TransferSegmentTableStruct t;
    SegmentOffsetLengthStruct s = getSegmentForDownloading(length);
    quint64 segmentStart = s.segmentBucketOffset * HASH_BUCKET_SIZE;
    t.segmentEnd = (s.segmentBucketOffset + s.segmentBucketCount) * HASH_BUCKET_SIZE;
    download->setSegmentStart(segmentStart);
    download->setSegmentEnd(t.segmentEnd);
    if (segmentStart != t.segmentEnd) // otherwise done
    {
        t.transferSegment = download;
        transferSegmentTable.insert(segmentStart, t);
        download->startDownloading();
    }
}

/*void DownloadTransfer::updateTransferSegmentTableRange(TransferSegment *segment, quint64 newStart, quint64 newEnd)
{
    if (transferSegmentTable.contains(segment->getSegmentStart()))
    {
        TransferSegmentTableStruct tsts;
        tsts.segmentEnd = newEnd;
        tsts.transferSegment = segment;
        transferSegmentTable.remove(segment->getSegmentStart());
        transferSegmentTable.insert(newStart, tsts);
    }
    else
    {
        qDebug() << "Eek! Tried to update misaligned download segment range! "
                 << "oldStart " << segment->getSegmentStart()
                 << "TTH " << TTH.toBase64();
    }
}*/

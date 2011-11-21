#include "downloadtransfer.h"

DownloadTransfer::DownloadTransfer()
{
    transferRate = 0;
    transferProgress = 0;
    bytesWrittenSinceUpdate = 0;
    status = TRANSFER_STATE_INITIALIZING;
    remoteHost = QHostAddress("0.0.0.0");

    transferRateCalculationTimer = new QTimer(this);
    connect(transferRateCalculationTimer, SIGNAL(timeout()), this, SLOT(transferRateCalculation()));
    transferRateCalculationTimer->setSingleShot(false);
    transferRateCalculationTimer->start(1000);
}

DownloadTransfer::~DownloadTransfer()
{
    delete transferRateCalculationTimer;
    QHashIterator<int, QByteArray*> itdb(downloadBucketTable);
    while (itdb.hasNext())
    {
        // TODO: save halwe buckets na files toe
        delete itdb.next();
    }
    QHashIterator<int, QByteArray*> ithb(downloadBucketHashLookupTable);
    while (ithb.hasNext())
        delete ithb.next();
}

void DownloadTransfer::incomingDataPacket(quint8 transferPacketType, quint64 &offset, QByteArray &data)
{
    // we are interested in which transfer protocol the packet is encoded with, since a download object may receive packets from various sources
    // transmitted with different transfer protocols.
    int bucketNumber = calculateBucketNumber(offset);
    if (!downloadBucketTable.contains(bucketNumber))
    {
        QByteArray *bucket = new QByteArray();
        downloadBucketTable.insert(bucketNumber, bucket);
    }
    if ((downloadBucketTable.value(bucketNumber)->length() + data.length()) > BUCKET_SIZE)
    {
        int bucketRemaining = BUCKET_SIZE - downloadBucketTable.value(bucketNumber)->length();
        downloadBucketTable.value(bucketNumber)->append(data.mid(0, bucketRemaining));
        if (!downloadBucketTable.contains(bucketNumber + 1))
        {
            QByteArray *nextBucket = new QByteArray(data.mid(bucketRemaining));
            downloadBucketTable.insert(bucketNumber + 1, nextBucket);
        }
        // there should be no else - if the next bucket exists and data is sticking over, there is an error,
        // since we segment on bucket boundaries. tth checksumming will catch the problems.
    }
    else
        downloadBucketTable.value(bucketNumber)->append(data);

    if (downloadBucketTable.value(bucketNumber)->length() == BUCKET_SIZE)
        emit hashBucketRequest(TTH, bucketNumber, downloadBucketTable.value(bucketNumber));
}

void DownloadTransfer::hashBucketReply(int &bucketNumber, QByteArray &bucketTTH)
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

void DownloadTransfer::TTHTreeReply(QByteArray &rootTTH, QByteArray &tree)
{
    while (tree.length() >= 28)
    {
        int bucketNumber = tree.mid(0, 4).toInt();
        QByteArray *bucketHash = new QByteArray(tree.mid(4, 24));
        tree.remove(0, 28);
        downloadBucketHashLookupTable.insert(bucketNumber, bucketHash);
    }
}

int DownloadTransfer::getTransferType()
{
    return TRANSFER_TYPE_DOWNLOAD;
}

void DownloadTransfer::startTransfer()
{
    status = TRANSFER_STATE_RUNNING; //TODO
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

// currently copied from uploadtransfer.
// we need the freedom to change this if necessary, therefore, it is not implemented in parent class.
void DownloadTransfer::transferRateCalculation()
{
    if ((status == TRANSFER_STATE_RUNNING) && (bytesWrittenSinceUpdate == 0))
        status = TRANSFER_STATE_STALLED;
    else if (status == TRANSFER_STATE_STALLED)
        status = TRANSFER_STATE_RUNNING;

    // snapshot the transfer rate as the amount of bytes written in the last second
    transferRate = bytesWrittenSinceUpdate;
    bytesWrittenSinceUpdate = 0;
}

int DownloadTransfer::calculateBucketNumber(quint64 fileOffset)
{
    return (int)(fileOffset >> 20);
}

void DownloadTransfer::flushBucketToDisk(int &bucketNumber)
{
    // TODO: decide where to store these files
    byte *tthBytes = new byte[TTH.length()];
    for (int i = 0; i < TTH.length(); i++)
        tthBytes[i] = TTH.at(i);
    QString tempFileName = base32Encode(tthBytes, TTH.length());
    delete tthBytes;
    tempFileName.append(".");
    tempFileName.append(QString::number(bucketNumber));

    QFile file(tempFileName);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(*downloadBucketTable.value(bucketNumber));
    else
    {
        // TODO: emit MISTAKE!, pause download
    }
    file.close();
}

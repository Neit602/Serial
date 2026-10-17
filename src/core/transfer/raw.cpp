// Raw binary send (PRD F19): the file's bytes, unframed, with optional
// pacing for a receiver that has no flow control.

#include "core/transfer/engine.h"

#include <QThread>

#include <cmath>

namespace portside::transfer {

void sendRaw(Job &job, const std::vector<std::unique_ptr<OutgoingFile>> &files)
{
    const int chunkSize = qMax(1, job.options.rawChunkSize);
    const int delayMs = int(std::lround(qMax(0.0, job.options.rawChunkDelay) * 1000.0));
    const int count = int(files.size());
    for (int index = 0; index < count; ++index) {
        OutgoingFile &file = *files[std::size_t(index)];
        job.beginFile(file.name(), file.size(), index + 1, count);
        while (!file.atEnd()) {
            job.link.checkCancelled();
            const QByteArray chunk = file.read(chunkSize);
            if (chunk.isEmpty())
                break;
            job.link.put(chunk);
            job.advance(chunk.size());
            // Sleep in short slices so a cancel is noticed promptly.
            for (int waited = 0; waited < delayMs; waited += 20) {
                job.link.checkCancelled();
                QThread::msleep(qMin(20, delayMs - waited));
            }
        }
        job.endFile(file.path(), file.size());
    }
}

} // namespace portside::transfer

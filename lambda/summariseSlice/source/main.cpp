#include <array>
#include <endian.h>
#include <fstream>
#include <iostream>

#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/platform/Environment.h>
#include <aws/core/utils/json/JsonSerializer.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/core/utils/stream/PreallocatedStreamBuf.h>
#include <aws/dynamodb/DynamoDBClient.h>
#include <aws/dynamodb/model/UpdateItemRequest.h>
#include <aws/lambda-runtime/runtime.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/GetObjectRequest.h>

#include "fast_atoi.h"
#include "GzipReader.h"
#include "stopwatch.h"

constexpr const char* TAG = "LAMBDA_ALLOC";
constexpr uint_fast32_t BGZIP_MAX_BLOCKSIZE = 65536;
constexpr uint_fast8_t BGZIP_BLOCK_START_LENGTH = 4;
constexpr const uint8_t BGZIP_BLOCK_START[BGZIP_BLOCK_START_LENGTH] = {0x1f, 0x8b, 0x08, 0x04};
constexpr uint_fast8_t BGZIP_FIELD_START_LENGTH = 4;
constexpr const uint8_t BGZIP_FIELD_START[BGZIP_FIELD_START_LENGTH] = {'B', 'C', 0x02, 0x00};
constexpr uint_fast16_t DOWNLOAD_SLICE_NUM = 4;  // Maximum number of concurrent downloads
constexpr uint_fast64_t MAX_DOWNLOAD_SLICE_SIZE = 100000000;
constexpr uint_fast8_t XLEN_OFFSET = 10;


struct VcfChunk
{
    const uint_fast64_t startCompressed;
    const uint_fast16_t startUncompressed;
    const uint_fast64_t endCompressed;
    const uint_fast16_t endUncompressed;
    VcfChunk(uint64_t virtualStart, uint64_t virtualEnd)
    :startCompressed(virtualStart >> 16), startUncompressed(virtualStart & 0xffff), endCompressed(virtualEnd >> 16), endUncompressed(virtualEnd & 0xffff) {}
};

// A non-copying iostream.
// See https://stackoverflow.com/questions/35322033/aws-c-sdk-uploadpart-times-out
// https://stackoverflow.com/questions/13059091/creating-an-input-stream-from-constant-memory
class StringViewStream: Aws::Utils::Stream::PreallocatedStreamBuf, public Aws::IOStream
{
    public:
    StringViewStream(uint8_t* data, size_t nbytes)
    :Aws::Utils::Stream::PreallocatedStreamBuf(data, nbytes), Aws::IOStream(this)
    {}
};


// class TestStream: StaticStreamBuff, public Aws::IOStream
// {
//     public:
//     TestStream(uint8_t* data, size_t nbytes)
//     :StaticStreamBuff(data, nbytes), Aws::IOStream(this)
//     {}

//     template <typename T>
//     StringViewStream& operator<< (const T &t) override
//     {
//         std::cout << "going in" << std::endl;
//         return Aws::IOStream::operator<< <T>(t);
//     }
// };

// void download(Aws::S3::S3Client const& s3Client, Aws::S3::Model::GetObjectRequest request)
// {
//     Aws::S3::Model::GetObjectOutcome response = s3Client.GetObject(request);
//     if (!response.IsSuccess())
//     {
//         std::cout << "Could not download data." << std::endl;
//     }
//     size_t totalBytes = response.GetResult().GetContentLength();
//     std::cout << "Finished download. Got " << totalBytes << " bytes." << std::endl;
// }


class Downloader
{
    Aws::S3::Model::GetObjectRequest m_request;
    Aws::S3::S3Client m_s3Client;
    std::thread m_thread;
    public:
    size_t downloadSize;
    Downloader(Aws::S3::S3Client const& s3Client, Aws::String const& bucket, Aws::String const& key, uint8_t* windowStart, size_t numBytes)
    :m_s3Client(s3Client), downloadSize(0)
    {
        m_request.SetBucket(bucket);
        m_request.SetKey(key);
        m_request.SetResponseStreamFactory([windowStart, numBytes]()
        {
            return Aws::New<StringViewStream>(TAG, windowStart, numBytes);
        });
    }

    static void download(Aws::S3::S3Client const& s3Client, Aws::S3::Model::GetObjectRequest request, uint_fast64_t firstByte, size_t numBytes)
    {
        Aws::String byteRange = "bytes=" + std::to_string(firstByte) + "-" + std::to_string(firstByte + numBytes - 1);
        request.SetRange(byteRange);
        std::cout << "Attempting to download s3://" << request.GetBucket() << "/" << request.GetKey() << " with byterange: \"" << byteRange << "\"" << std::endl;
        Aws::S3::Model::GetObjectOutcome response = s3Client.GetObject(request);
        if (!response.IsSuccess())
        {
            std::cout << "Could not download data." << std::endl;
        }
        size_t totalBytes = response.GetResult().GetContentLength();
        std::cout << "Finished download. Got " << totalBytes << " bytes." << std::endl;
    }

    void startDownload(uint_fast64_t firstByte, size_t numBytes)
    {
        if (numBytes > 0)
        {
            m_thread = std::thread(download, m_s3Client, m_request, firstByte, numBytes);
            downloadSize = numBytes;
        }
    }

    size_t join()
    {
        if (m_thread.joinable())
        {
            m_thread.join();
            return downloadSize;
        } else {
            return 0;
        }
    }
};

// See http://samtools.github.io/hts-specs/SAMv1.pdf section 4.1 "The BGZF compression format"
class VcfChunkReader
{
    public:
    const size_t startCompressed;
    const size_t totalBytes;
    const size_t numBytes;
    size_t requestedBytes;
    uint8_t* gzipBytes;
    size_t blockStart;
    size_t nextBlockStart;
    const size_t finalBlock;
    const uint_fast32_t finalUncompressed;
    unsigned char* inflateWindow;
    char* uncompressedChars;
    char* readBufferStart;
    size_t readBufferLength;
    Aws::Vector <char> readAltBuffer;
    uint_fast64_t charPos;
    z_stream zStream;
    uint_fast16_t currentSlice;
    Aws::Vector <Downloader> downloaders;
    size_t windowIndex;
    size_t windowStart;
    size_t totalCSize;
    uint_fast64_t totalUSize;
    uint_fast16_t blockXlen;
    size_t blockCompressedStart;
    uint_fast32_t blockChars;
    stop_watch stopWatch;
    uint reads;

    public:
    VcfChunkReader(Aws::String bucket, Aws::String key, Aws::S3::S3Client const& s3Client, VcfChunk chunk)
    :startCompressed(chunk.startCompressed),
     totalBytes(BGZIP_MAX_BLOCKSIZE + chunk.endCompressed - startCompressed),
     numBytes(BGZIP_MAX_BLOCKSIZE + std::min(DOWNLOAD_SLICE_NUM * MAX_DOWNLOAD_SLICE_SIZE, totalBytes)),
     requestedBytes(0), gzipBytes(new uint8_t[numBytes]),
     nextBlockStart(BGZIP_MAX_BLOCKSIZE), finalBlock(chunk.endCompressed - startCompressed),
     finalUncompressed(chunk.endUncompressed), uncompressedChars(new char[BGZIP_MAX_BLOCKSIZE]),
     readBufferStart(uncompressedChars), readBufferLength(0),
     charPos(0), currentSlice(1), windowIndex(0), windowStart(BGZIP_MAX_BLOCKSIZE), totalCSize(0),
     totalUSize(0), blockChars(0), stopWatch(), reads(0)
    {
        do
        {
            downloaders.push_back(Downloader(s3Client, bucket, key, gzipBytes + BGZIP_MAX_BLOCKSIZE + requestedBytes, bytesToRequest()));
            downloadNext();
            windowIndex++;
        } while (requestedBytes + BGZIP_MAX_BLOCKSIZE < numBytes);
        std::cout << "Downloading " << totalBytes << " bytes using " << windowIndex << " additional threads." << std::endl;
        windowIndex = 0;
        downloaders[windowIndex].join();
        getNextBlock();

        //Initialise z_stream
        zStream.zalloc = Z_NULL;
        zStream.zfree = Z_NULL;
        zStream.opaque = Z_NULL;

        seek(chunk.startUncompressed);
    }

    void addToReadAltBuffer(char* buf, size_t length, size_t& altBufferPos)
    {
        if (readAltBuffer.size() < altBufferPos + length)
        {
            readAltBuffer.resize(altBufferPos + length);
        }
        readBufferLength += length;
        std::copy(buf, buf + length, readAltBuffer.begin() + altBufferPos);
        altBufferPos += length;
    }

    void downloadNext()
    {
        downloaders[windowIndex].startDownload(startCompressed + requestedBytes, bytesToRequest());
        requestedBytes += bytesToRequest();
    }


    uint_fast8_t get8(uint8_t* buf)
    {
        return static_cast<uint_fast8_t>(*buf);
    }

    uint_fast16_t get16(uint8_t* buf)
    {
        uint16_t value;
        memcpy(&value, buf, 2);
        return static_cast<uint_fast16_t>(le16toh(value));
    }

    uint_fast32_t get32(uint8_t* buf)
    {
        uint32_t value;
        memcpy(&value, buf, 4);
        return static_cast<uint_fast32_t>(le32toh(value));
    }

    void getBlockDetails()
    {
        if (memcmp(gzipBytes + blockStart, BGZIP_BLOCK_START, BGZIP_BLOCK_START_LENGTH) != 0)
        {
            std::cout << "Block " << blockStart << " does not start with correct bytes" << std::endl;
        }
        blockXlen = get16(gzipBytes + blockStart + XLEN_OFFSET);
        blockCompressedStart = blockStart + XLEN_OFFSET + 2 + blockXlen;

        bool bSizeFound = false;
        uint8_t* fieldStart = gzipBytes + blockStart + XLEN_OFFSET + 2;
        do {
            if (memcmp(fieldStart, BGZIP_FIELD_START, BGZIP_FIELD_START_LENGTH) == 0)
            {
                nextBlockStart = blockStart + get16(fieldStart + BGZIP_FIELD_START_LENGTH) + 1;
                totalCSize += nextBlockStart - blockStart;
                bSizeFound = true;
                break;
            }
            else {
                fieldStart += get16(fieldStart + 2);
            }
        } while (fieldStart < gzipBytes + blockCompressedStart);
        if (!bSizeFound)
        {
            std::cout << "Block " << blockStart << " does not have extra field for BSIZE" << std::endl;
        }
        blockChars = moreBlocks() ? get32(gzipBytes + nextBlockStart - 4) : finalUncompressed;
        totalUSize += blockChars;
    }

    bool getNextBlock()
    {
        if (moreBlocks() && charPos >= blockChars)
        {
            charPos -= blockChars;
            blockStart = nextBlockStart;
            size_t nextWindow = windowStart + downloaders[windowIndex].downloadSize;
            // Check if the next download window is coming up
            if (nextWindow < blockStart + BGZIP_MAX_BLOCKSIZE)
            {
                if (windowIndex + 1 == downloaders.size())
                {
                    // If this is the final window in the buffer, move back to the start
                    memcpy(gzipBytes + BGZIP_MAX_BLOCKSIZE + blockStart - nextWindow, gzipBytes + blockStart, nextWindow - blockStart);
                    blockStart = BGZIP_MAX_BLOCKSIZE + blockStart - nextWindow;
                    downloadNext();
                    windowIndex = 0;
                    windowStart = BGZIP_MAX_BLOCKSIZE;
                    downloaders[windowIndex].join();
                } else if (nextWindow < blockStart)
                {
                    // We're clear of the current download window, so we can overwrite it and move to the next.
                    downloadNext();
                    windowStart = nextWindow;
                    windowIndex++;
                } else {
                    downloaders[windowIndex+1].join();
                }
            }
            getBlockDetails();
        }
        return keepReading();
    }

    size_t getReadLength()
    {
        return readBufferLength;
    }

    const char* getReadStart()
    {
        return readBufferStart;
    }

    bool keepReading()
    {
        return moreBlocks() || charPos < blockChars;
    }

    bool moreBlocks()
    {
        return totalCSize <= finalBlock;
    }

    template <bool first=false>
    void readBlock()
    {
        zStream.next_in = gzipBytes + blockCompressedStart;
        zStream.avail_in = static_cast<uint>(nextBlockStart - blockStart - blockXlen - 20);
        zStream.next_out = reinterpret_cast<unsigned char*>(uncompressedChars);
        zStream.avail_out = static_cast<uint>(blockChars);
        if (first)
        {
            inflateInit2(&zStream, -15);
        } else {
            inflateReset(&zStream);
        }
        stopWatch.start();
        inflate(&zStream, Z_FINISH);
        stopWatch.stop();
        if (++reads <= 10 || (reads > 15000 && reads <= 15010))
        {
            std::cout << "Inflate took: " << stopWatch << " to inflate " << nextBlockStart - blockStart - blockXlen - 20 << " bytes into " << blockChars << " bytes on read " << reads << std::endl;
        }
    }

    template <char charA, char charB>
    char readPastChars()
    {
        readBufferStart = uncompressedChars + charPos;
        readBufferLength = 0;
        size_t altBufferPos = 0;
        uint_fast64_t startCharPos;
        do {
            startCharPos = charPos;
            while (charPos < blockChars)
            {
                if (uncompressedChars[charPos] == charA || uncompressedChars[charPos] == charB)
                {
                    if (readBufferStart != nullptr)
                    {
                        readBufferLength += charPos - startCharPos;
                    } else {
                        addToReadAltBuffer(uncompressedChars + startCharPos, charPos - startCharPos, altBufferPos);
                        readBufferStart = readAltBuffer.data();
                    }
                    return uncompressedChars[charPos++];
                } else {
                    charPos++;
                }
            }
            if (!moreBlocks())
            {
                return '\0';
            }
            addToReadAltBuffer(uncompressedChars + startCharPos, blockChars - startCharPos, altBufferPos);
            readBufferStart = nullptr;
            getNextBlock();
            readBlock();
        } while (true);
    }

    size_t bytesToRequest()
    {
        return std::min(MAX_DOWNLOAD_SLICE_SIZE, totalBytes - requestedBytes);
    }

    template <size_t N=1, char delim>
    bool skipPast()
    {
        size_t num = N;
        do {
            while (charPos < blockChars)
            {
                if (uncompressedChars[charPos++] == delim && ((N==1)||(--num==0)))
                {
                    return true;
                }
            }
            if (!moreBlocks())
            {
                return false;
            }
            getNextBlock();
            readBlock();
        } while (true);
    }

    uint_fast64_t skipPastAndCountChars(char delim)
    {
        uint_fast64_t numChars = 0;
        do {
            while (charPos < blockChars)
            {
                numChars += ((uncompressedChars[charPos] == '\t') || (uncompressedChars[charPos] == '/') || (uncompressedChars[charPos] == '|') || (uncompressedChars[charPos] == ';'));
                if (uncompressedChars[charPos++] == delim)
                {
                    return numChars;
                }
            }
            if (!moreBlocks())
            {
                return numChars;
            }
            getNextBlock();
            readBlock();
        } while (true);
    }

    bool seek(uint_fast64_t numChars)
    {
        charPos += numChars;
        if (charPos < blockChars)
        {
            return keepReading();
        }
        while ((blockChars <= charPos) && getNextBlock())
        {
            // Skip through blocks until we reach the desired charPos
        }
        readBlock(); 
        return keepReading();
    }

    ~VcfChunkReader()
    {
        inflateEnd(&zStream);
        delete[] uncompressedChars;
        delete[] gzipBytes;
    }
};


void add_counts(VcfChunkReader& reader, uint_fast64_t& numCalls, uint_fast64_t& numVariants)
{
    constexpr const char* acTag = "AC=";
    constexpr const char* anTag = "AN=";
    bool foundAc = false;
    bool foundAn = false;
    // Skip to info section, we want AC and AN
    reader.skipPast<7, '\t'>();
    do {
        const char lastChar = reader.readPastChars<';', '\t'>();
        if (lastChar == '\0')
        {
            // EOF in the middle of INFO, don't include this
            break;
        }
        const size_t numChars = reader.getReadLength();
        if (numChars >= 4)
        {
            // Could be AC=x or AN=x
            const char* firstChar_p = reader.getReadStart();
            if (memcmp(firstChar_p, acTag, 3) == 0)
            {
                foundAc = true;
                numVariants += 1;
                for (size_t j = 3; j < numChars; ++j)
                {
                    // Number of variants should be 1 more than the number of commas
                    if (*(firstChar_p + j) == ',')
                    {
                        numVariants += 1;
                    }
                }
            } else if (memcmp(firstChar_p, anTag, 3) == 0) {
                foundAn = true;
                numCalls += atoui64(firstChar_p+3, (uint8_t)numChars-3);
            } else {
                std::cout << "Found unrecognised INFO field: \"" << Aws::String(firstChar_p, numChars) << "\" with lastChar: \"" << lastChar << "\" and charPos: " << reader.charPos << std::endl;
            }
        } else {
            std::cout << "Found short unrecognised INFO field: \"" << Aws::String(reader.getReadStart(), numChars) << "\" with lastChar: \"" << lastChar << "\" and charPos: " << reader.charPos << std::endl;
        }
        if (lastChar == '\t' && !(foundAc && foundAn))
        {
            std::cout << "Did not find either AC or AN. AC found: " << foundAc << ". AN found: " << foundAn << std::endl;
            break;
        }
    } while (!(foundAc && foundAn));
}

Aws::String bundle_response(Aws::String const& body, int statusCode)
{
    Aws::String outputString = "{\"headers\": {\"Access-Control-Allow-Origin\": \"*\"}, \"statusCode\": ";
    outputString.append(std::to_string(statusCode));
    outputString.append(", \"body\": \"");
    for (char c : body)
    {
        if (c == '"' || c == '\\')
        {
            outputString.push_back('\\');
        }
        outputString.push_back(c);
    }
    outputString.append("\"}");
    return outputString;
}


Aws::String get_message_string(aws::lambda_runtime::invocation_request const& req)
{
    Aws::Utils::Json::JsonValue json(req.payload);
    return json.View().GetArray("Records").GetItem(0).GetObject("Sns").GetString("Message");
}

static aws::lambda_runtime::invocation_response my_handler(aws::lambda_runtime::invocation_request const& req,
    Aws::S3::S3Client const& s3Client)
{
    Aws::String messageString = get_message_string(req);
    std::cout << "Message is: " << messageString << std::endl;
    Aws::Utils::Json::JsonValue message(messageString);
    Aws::Utils::Json::JsonView messageView = message.View();
    Aws::String location = messageView.GetString("location");
    int64_t virtualStart = messageView.GetInt64("virtual_start");
    int64_t virtualEnd = messageView.GetInt64("virtual_end");
    VcfChunk chunk(virtualStart, virtualEnd);

    // Get bucket and key from location
    Aws::String bucket = "";
    Aws::String key = "";
    for (size_t j = 5; j < location.length(); ++j)
    {
        if (location[j] == '/')
        {
            bucket = location.substr(5, j-5);
            key = location.substr(j+1);
            break;
        }
    }
    stop_watch s = stop_watch();
    s.start();
    VcfChunkReader vcfChunkReader(bucket, key, s3Client, chunk);
    std::cout << "Loaded Reader" << std::endl;
    vcfChunkReader.readBlock<true>();
    std::cout << "Read block with " << vcfChunkReader.zStream.total_out << " bytes output." << std::endl;
    uint_fast64_t numCalls = 0;
    uint_fast64_t numVariants = 0;
    add_counts(vcfChunkReader, numCalls, numVariants);
    uint_fast64_t skip_size = 2 * vcfChunkReader.skipPastAndCountChars('\n');
    std::cout << "vcfChunkReader skip_size: " << skip_size << std::endl;
    std::cout << "First record numVariants: " << numVariants << " numCalls: " << numCalls << std::endl;
    uint_fast32_t records = 1;
    while (vcfChunkReader.keepReading())
    {

        add_counts(vcfChunkReader, numCalls, numVariants);
        vcfChunkReader.seek(skip_size);
        vcfChunkReader.skipPast<1, '\n'>();
        records += 1;
    }
    s.stop();
    std::cout << "Finished processing " << vcfChunkReader.totalBytes << " bytes in " << s << " (" << 1000 * vcfChunkReader.totalBytes / s.nanoseconds << "MB/s)" << std::endl;
    std::cout << "vcfChunkReader read " << vcfChunkReader.reads << " blocks completely, found compressed size: " << vcfChunkReader.totalCSize << " and uncompressed size: " << vcfChunkReader.totalUSize << " with records: " << records << std::endl;
    std::cout << "numVariants: " << numVariants << ", numCalls: " << numCalls << std::endl;
    return aws::lambda_runtime::invocation_response::success(bundle_response("Success", 200), "application/json");
}

int main()
{
    Aws::SDKOptions options;
    Aws::InitAPI(options);
    {
        Aws::Client::ClientConfiguration config;
        config.region = Aws::Environment::GetEnv("AWS_REGION");
        config.caFile = "/etc/pki/tls/certs/ca-bundle.crt";

        auto credentialsProvider = Aws::MakeShared<Aws::Auth::EnvironmentAWSCredentialsProvider>(TAG);
        Aws::S3::S3Client s3Client(credentialsProvider, config);
        auto handler_fn = [&s3Client](aws::lambda_runtime::invocation_request const& req) {
            std::cout << "Event Recived: " << req.payload << std::endl;
            return my_handler(req, s3Client);
            std::cout.flush();
        };
        aws::lambda_runtime::run_handler(handler_fn);
    }
    Aws::ShutdownAPI(options);
    return 0;
}
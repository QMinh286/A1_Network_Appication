/*
    Reliability and Flow Control Example
    From "Networking for Game Programmers" - http://www.gaffer.org/networking-for-game-programmers
    Author: Glenn Fiedler <gaffer@gaffer.org>
*/

#include <iostream>
#include <fstream>
#include <string>
#include <vector>

#include "Net.h"
#include "md5.h"

//#define SHOW_ACKS

using namespace std;
using namespace net;

const int ServerPort = 30000;
const int ClientPort = 30001;
const int ProtocolId = 0x11223344;
const float DeltaTime = 1.0f / 30.0f;
const float SendRate = 1.0f / 30.0f;
const float TimeOut = 10.0f;
const int PacketSize = 256;

class FlowControl
{
public:
    FlowControl()
    {
        printf("flow control initialized\n");
        Reset();
    }

    void Reset()
    {
        mode = Bad;
        penalty_time = 4.0f;
        good_conditions_time = 0.0f;
        penalty_reduction_accumulator = 0.0f;
    }

    void Update(float deltaTime, float rtt)
    {
        const float RTT_Threshold = 250.0f;

        if (mode == Good)
        {
            if (rtt > RTT_Threshold)
            {
                printf("*** dropping to bad mode ***\n");
                mode = Bad;
                if (good_conditions_time < 10.0f && penalty_time < 60.0f)
                {
                    penalty_time *= 2.0f;
                    if (penalty_time > 60.0f)
                        penalty_time = 60.0f;
                    printf("penalty time increased to %.1f\n", penalty_time);
                }
                good_conditions_time = 0.0f;
                penalty_reduction_accumulator = 0.0f;
                return;
            }

            good_conditions_time += deltaTime;
            penalty_reduction_accumulator += deltaTime;

            if (penalty_reduction_accumulator > 10.0f && penalty_time > 1.0f)
            {
                penalty_time /= 2.0f;
                if (penalty_time < 1.0f)
                    penalty_time = 1.0f;
                printf("penalty time reduced to %.1f\n", penalty_time);
                penalty_reduction_accumulator = 0.0f;
            }
        }

        if (mode == Bad)
        {
            if (rtt <= RTT_Threshold)
                good_conditions_time += deltaTime;
            else
                good_conditions_time = 0.0f;

            if (good_conditions_time > penalty_time)
            {
                printf("*** upgrading to good mode ***\n");
                good_conditions_time = 0.0f;
                penalty_reduction_accumulator = 0.0f;
                mode = Good;
                return;
            }
        }
    }

    float GetSendRate()
    {
        return mode == Good ? 30.0f : 10.0f;
    }

private:
    enum Mode
    {
        Good,
        Bad
    };

    Mode mode;
    float penalty_time;
    float good_conditions_time;
    float penalty_reduction_accumulator;
};

// ----------------------------------------------

int main(int argc, char* argv[])
{
    // parse command line
    enum Mode
    {
        Client,
        Server
    };

    Mode mode = Server;
    Address address;
    string fileName;

    /*
        Checks if we are running as a client or server.
        When running as the client the system grabs file path we are sending and IP address of the server.
    */
    if (argc >= 2)
    {
        int a, b, c, d;
#pragma warning(suppress : 4996)
        if (sscanf(argv[1], "%d.%d.%d.%d", &a, &b, &c, &d))
        {
            mode = Client;
            address = Address(a, b, c, d, ServerPort); // grabs the server IP address
            fileName = argv[2]; // grabs file path to the file we are sending. 
        }
    }

    // initialize
    if (!InitializeSockets())
    {
        printf("failed to initialize sockets\n");
        return 1;
    }

    ReliableConnection connection(ProtocolId, TimeOut);

    const int port = mode == Server ? ServerPort : ClientPort;

    if (!connection.Start(port))
    {
        printf("could not start connection on port %d\n", port);
        return 1;
    }

    // ------------------------------
    // Client Side: Send Metadata
    // ------------------------------
    if (mode == Client)
    {
        std::ifstream file;
        file.open(fileName, std::ios::binary);
        if (!file)
        {
            // error opening the file
            printf("Error: could not open \"%s\". Please try again.\n", fileName.c_str());
            return 0;
        }
        else // file opened successfully
        {
            // connects to the server
            connection.Connect(address);

            // grabs the file size from the file metadata
            file.seekg(0, std::ios::end);              // this moves the file pointer to the end of the file
            std::streamsize fileSize = file.tellg();   // this grabs the size of the file
            file.seekg(0, std::ios::beg);              // moves file pointer back to start of file
            // sends the file size to the server
            connection.SendPacket(reinterpret_cast<unsigned char*>(&fileSize), sizeof(fileSize));

            // reads the file into a buffer and creates the MD5 hash
            vector<char> fileBuffer(fileSize);
            file.read(fileBuffer.data(), fileSize);
            MD5 md5;
            md5.update(fileBuffer.data(), fileSize);
            string fileHash = md5.finalize().hexdigest();
            // sends the MD5 hash to the server
            connection.SendPacket(reinterpret_cast<const unsigned char*>(fileHash.c_str()), fileHash.length());

            // sends the file name to the server
            connection.SendPacket(reinterpret_cast<const unsigned char*>(fileName.c_str()), fileName.length());

            printf("Client sent metadata:\n");
            printf("  File size: %lld bytes\n", (long long)fileSize);
            printf("  MD5 hash: %s\n", fileHash.c_str());
            printf("  File name: %s\n", fileName.c_str());
        }
    }
    // ------------------------------
    // Server Side: Receive Metadata
    // ------------------------------
    else // mode == Server
    {
        connection.Listen();
        // wait until connected
        while (!connection.IsConnected())
        {
            connection.Update(DeltaTime);
            net::wait(DeltaTime);
        }

        // Variables to track metadata reception
        bool fileSizeReceived = false;
        bool hashReceived = false;
        bool fileNameReceived = false;
        std::streamsize fileSize = 0;
        std::string fileHash;
        std::string fileName;

        // Loop until all metadata packets are received
        while (!(fileSizeReceived && hashReceived && fileNameReceived))
        {
            // receive packets
            unsigned char packet[PacketSize];
            int bytes_read = connection.ReceivePacket(packet, sizeof(packet));
            if (bytes_read > 0)
            {
                if (!fileSizeReceived)
                {
                    // first packet: file size (sent as binary)
                    fileSize = *reinterpret_cast<std::streamsize*>(packet);
                    printf("Received file size: %lld bytes\n", (long long)fileSize);
                    fileSizeReceived = true;
                }
                else if (!hashReceived)
                {
                    // second packet: MD5 hash string
                    fileHash = std::string(reinterpret_cast<char*>(packet), bytes_read);
                    printf("Received MD5 hash: %s\n", fileHash.c_str());
                    hashReceived = true;
                }
                else if (!fileNameReceived)
                {
                    // third packet: file name
                    fileName = std::string(reinterpret_cast<char*>(packet), bytes_read);
                    printf("Received file name: %s\n", fileName.c_str());
                    fileNameReceived = true;
                }
            }
            connection.Update(DeltaTime);
            net::wait(DeltaTime);
        }
    }

    ShutdownSockets();

    return 0;
}

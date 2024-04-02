// ReSharper disable CppClangTidyClangDiagnosticInvalidUtf8
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <WinSock2.h>
#include <winsock.h>
#include <Windows.h>
#include <iostream>
#include <thread>
#include <vector>
#include <unordered_set>
#include <mutex>
#include <atomic>
#include <chrono>
#include <queue>
#include <array>
#include <memory>

using namespace std;
using namespace chrono;

extern HWND hWnd;

static constexpr int MAX_TEST = 50000;
static constexpr int MAX_CLIENTS = MAX_TEST * 2;
static constexpr int INVALID_ID = -1;
static constexpr int MAX_PACKET_SIZE = 255;
static constexpr int MAX_BUFF_SIZE = 255;

#pragma comment (lib, "ws2_32.lib")

#include "../../iocp_server - NPC_AI/st_iocp_server/protocol.h"
;
HANDLE g_hiocp;

enum OPTYPE { OP_SEND, OP_RECV, OP_DO_MOVE };

high_resolution_clock::time_point last_connect_time;

struct OverlappedEx
{
    WSAOVERLAPPED over;
    WSABUF wsabuf;
    unsigned char IOCP_buf[MAX_BUFF_SIZE];
    OPTYPE event_type;
    int event_target;
};

struct CLIENT
{
    int id;
    int x;
    int y;
    atomic_bool connected;

    SOCKET client_socket;
    OverlappedEx recv_over;
    unsigned char packet_buf[MAX_PACKET_SIZE];
    int prev_packet_data;
    int curr_packet_size;
    high_resolution_clock::time_point last_move_time;
};

array<int, MAX_USER> client_map;
array<CLIENT, MAX_USER> g_clients;
atomic_int num_connections;
atomic_int client_to_close;
atomic_int active_clients;

int global_delay; // ms단위, 1000이 넘으면 클라이언트 증가 종료

vector<thread *> worker_threads;
thread test_thread;

float point_cloud[MAX_TEST * 2];

// 나중에 NPC까지 추가 확장 용
struct ALIEN
{
    int id;
    int x, y;
    int visible_count;
};

void error_display(const char *msg, int err_no)
{
    WCHAR *lpMsgBuf;
    FormatMessage(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM,
        nullptr, err_no,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPTSTR)&lpMsgBuf, 0, nullptr);
    std::cout << msg;
    std::wcout << L"에러" << lpMsgBuf << std::endl;

    MessageBox(hWnd, lpMsgBuf, L"ERROR", 0);
    LocalFree(lpMsgBuf);
    // while (true);
}

void DisconnectClient(int ci)
{
    bool status = true;
    if (true == atomic_compare_exchange_strong(&g_clients[ci].connected, &status, false))
    {
        closesocket(g_clients[ci].client_socket);
        --active_clients;
    }
    // cout << "Client [" << ci << "] Disconnected!\n";
}

void SendPacket(int cl, void *packet)
{

    int psize = reinterpret_cast<unsigned char *>(packet)[0];
    int ptype = reinterpret_cast<unsigned char *>(packet)[1];
    auto over = new OverlappedEx;
    over->event_type = OP_SEND;
    memcpy(over->IOCP_buf, packet, psize);
    ZeroMemory(&over->over, sizeof(over->over));
    over->wsabuf.buf = reinterpret_cast<CHAR *>(over->IOCP_buf);
    over->wsabuf.len = psize;
    int ret = WSASend(g_clients[cl].client_socket, &over->wsabuf, 1, nullptr, 0,
                      &over->over, nullptr);
    if (0 != ret)
    {
        int err_no = WSAGetLastError();
        if (WSA_IO_PENDING != err_no)
            error_display("Error in SendPacket:", err_no);
    }
    // std::cout << "Send Packet [" << ptype << "] To Client : " << cl << std::endl;
}

void ProcessPacket(int ci, unsigned char packet[])
{
    switch (packet[1])
    {
    case S2C_MOVE_PLAYER:
    {
        auto move_packet = reinterpret_cast<s2c_move_player *>(packet);
        if (move_packet->id < MAX_USER)
        {
            int my_id = client_map[move_packet->id];
            if (-1 != my_id)
            {
                g_clients[my_id].x = move_packet->x;
                g_clients[my_id].y = move_packet->y;
            }
            if (ci == my_id)
            {
                if (0 != move_packet->move_time)
                {
                    auto d_ms = duration_cast<milliseconds>(high_resolution_clock::now().time_since_epoch()).count() -
                        move_packet->move_time;

                    if (global_delay < d_ms)
                        global_delay++;
                    else
                        if (global_delay > d_ms)
                            global_delay--;
                }
            }
        }
    }
    break;
    case S2C_ADD_PLAYER:
        break;
    case S2C_REMOVE_PLAYER:
        break;
    case S2C_LOGIN_OK:
    {
        g_clients[ci].connected = true;
        ++active_clients;
        auto login_packet = reinterpret_cast<s2c_login_ok *>(packet);
        int my_id = ci;
        client_map[login_packet->id] = my_id;
        g_clients[my_id].id = login_packet->id;
        g_clients[my_id].x = login_packet->x;
        g_clients[my_id].y = login_packet->y;

        //cs_packet_teleport t_packet;
        //t_packet.size = sizeof(t_packet);
        //t_packet.type = CS_TELEPORT;
        //SendPacket(my_id, &t_packet);
    }
    break;
    default: MessageBox(hWnd, L"Unknown Packet Type", L"ERROR", 0);
        while (true);
    }
}

void Worker_Thread()
{
    while (true)
    {
        DWORD io_size;
        unsigned long long ci;
        OverlappedEx *over;
        BOOL ret = GetQueuedCompletionStatus(g_hiocp, &io_size, &ci,
                                             reinterpret_cast<LPWSAOVERLAPPED *>(&over), INFINITE);
        // std::cout << "GQCS :";
        int client_id = static_cast<int>(ci);
        if (FALSE == ret)
        {
            int err_no = WSAGetLastError();
            if (64 == err_no)
                DisconnectClient(client_id);
            else
            {
                // error_display("GQCS : ", WSAGetLastError());
                DisconnectClient(client_id);
            }
            if (OP_SEND == over->event_type)
                delete over;
        }
        if (0 == io_size)
        {
            DisconnectClient(client_id);
            continue;
        }
        if (OP_RECV == over->event_type)
        {
            //std::cout << "RECV from Client :" << ci;
            //std::cout << "  IO_SIZE : " << io_size << std::endl;
            unsigned char *buf = g_clients[ci].recv_over.IOCP_buf;
            unsigned psize = g_clients[ci].curr_packet_size;
            unsigned pr_size = g_clients[ci].prev_packet_data;
            while (io_size > 0)
            {
                if (0 == psize)
                    psize = buf[0];
                if (io_size + pr_size >= psize)
                {
                    // 지금 패킷 완성 가능
                    unsigned char packet[MAX_PACKET_SIZE];
                    memcpy(packet, g_clients[ci].packet_buf, pr_size);
                    memcpy(packet + pr_size, buf, psize - pr_size);
                    ProcessPacket(static_cast<int>(ci), packet);
                    io_size -= psize - pr_size;
                    buf += psize - pr_size;
                    psize = 0;
                    pr_size = 0;
                }
                else
                {
                    memcpy(g_clients[ci].packet_buf + pr_size, buf, io_size);
                    pr_size += io_size;
                    io_size = 0;
                }
            }
            g_clients[ci].curr_packet_size = psize;
            g_clients[ci].prev_packet_data = pr_size;
            DWORD recv_flag = 0;
            int ret = WSARecv(g_clients[ci].client_socket,
                              &g_clients[ci].recv_over.wsabuf, 1,
                              nullptr, &recv_flag, &g_clients[ci].recv_over.over, nullptr);
            if (SOCKET_ERROR == ret)
            {
                int err_no = WSAGetLastError();
                if (err_no != WSA_IO_PENDING)
                {
                    //error_display("RECV ERROR", err_no);
                    DisconnectClient(client_id);
                }
            }
        }
        else if (OP_SEND == over->event_type)
        {
            if (io_size != over->wsabuf.len)
            {
                // std::cout << "Send Incomplete Error!\n";
                DisconnectClient(client_id);
            }
            delete over;
        }
        else if (OP_DO_MOVE == over->event_type)
        {
            // Not Implemented Yet
            delete over;
        }
        else
        {
            std::cout << "Unknown GQCS event!\n";
            while (true);
        }
    }
}

constexpr int DELAY_LIMIT = 1000;
constexpr int DELAY_LIMIT2 = 1500;
constexpr int ACCEPT_DELY = 50;

// 상수 정의
constexpr int DELAY_THRESHOLD = 10;
constexpr auto SERVER_IP = "127.0.0.1";

// 전역 변수 선언
int DELAY_MULTIPLIER = 10; // 연결 시도 간격을 조절하는 변수
static int max_limit = INT_MAX;  // 최대 연결 가능한 클라이언트 수
static bool increasing = true;   // 클라이언트 수 증가 여부

// 함수 선언
bool initialize_client_socket(SOCKET &clientSocket);
void connect_new_client();
void attempt_to_disconnect_client();
void adjust_delay_multiplier(const int time_delay, int &delay_multiplier);

void adjust_number_of_client()
{
    if (active_clients >= MAX_TEST || num_connections >= MAX_USER)
        return;

    const auto duration = high_resolution_clock::now() - last_connect_time;
    if (milliseconds(ACCEPT_DELY * DELAY_MULTIPLIER) > duration)
        return;

    const int delay = global_delay;
    adjust_delay_multiplier(delay, DELAY_MULTIPLIER);

    if (max_limit - (max_limit / 20) < active_clients)
        return;

    increasing = true;
    connect_new_client();
}

void adjust_delay_multiplier(const int time_delay, int &delay_multiplier)
{
    if (time_delay > DELAY_LIMIT2) // 딜레이 1.5초 초과되면 
    {
        attempt_to_disconnect_client();
    }
    else if (time_delay > DELAY_LIMIT) // 딜레이 1.0초 초과 1.5초 미만이면
    {
        delay_multiplier = DELAY_THRESHOLD; // 클라이언트 접속 속도를 늦춤
    }
}

void connect_new_client()
{
    SOCKET clientSocket;
    if (!initialize_client_socket(clientSocket))
    {
        std::cerr << "Failed to initialize client socket." << std::endl;
        return;
    }

    // 클라이언트 소켓을 사용하여 연결 및 데이터 전송 코드 추가 위치
    ++num_connections;
}

bool initialize_client_socket(SOCKET &clientSocket)
{
    clientSocket = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (clientSocket == INVALID_SOCKET)
    {
        error_display("WSASocketW failed", WSAGetLastError());
        return false;
    }

    sockaddr_in serverAddr;
    ZeroMemory(&serverAddr, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(SERVER_PORT);
    serverAddr.sin_addr.s_addr = inet_addr(SERVER_IP);

    int result = WSAConnect(clientSocket, (sockaddr *)&serverAddr, sizeof(serverAddr), nullptr, nullptr, nullptr,
                            nullptr);
    if (result != 0)
    {
        error_display("WSAConnect failed", WSAGetLastError());
        closesocket(clientSocket);
        return false;
    }
    return true;
}

void attempt_to_disconnect_client()
{
    // 클라이언트 연결 해제 시도 코드
}

void Test_Thread()
{
    while (true)
    {
        //Sleep(max(20, global_delay));
        adjust_number_of_client();

        for (int i = 0; i < num_connections; ++i)
        {
            if (false == g_clients[i].connected)
                continue;
            if (g_clients[i].last_move_time + 1s > high_resolution_clock::now())
                continue;
            g_clients[i].last_move_time = high_resolution_clock::now();
            c2s_move my_packet;
            my_packet.size = sizeof(my_packet);
            my_packet.type = C2S_MOVE;
            switch (rand() % 4)
            {
            case 0:
                my_packet.dr = D_N;
                break;
            case 1:
                my_packet.dr = D_S;
                break;
            case 2:
                my_packet.dr = D_W;
                break;
            case 3:
                my_packet.dr = D_E;
                break;
            }
            my_packet.move_time = static_cast<unsigned>(duration_cast<milliseconds>(
                high_resolution_clock::now().time_since_epoch()).count());
            SendPacket(i, &my_packet);
        }
    }
}

void ShutdownNetwork()
{
    test_thread.join();
    for (auto pth : worker_threads)
    {
        pth->join();
        delete pth;
    }
}

void InitializeNetwork()
{
    for (auto &cl : g_clients)
    {
        cl.connected = false;
        cl.id = INVALID_ID;
    }

    for (auto &cl : client_map)
        cl = -1;
    num_connections = 0;
    last_connect_time = high_resolution_clock::now();

    WSADATA wsadata;
    WSAStartup(MAKEWORD(2, 2), &wsadata);

    g_hiocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, NULL, 0);

    for (int i = 0; i < 6; ++i)
        worker_threads.push_back(new std::thread{Worker_Thread});

    test_thread = thread{Test_Thread};
    ShutdownNetwork();
}

void Do_Network()
{
}

void GetPointCloud(int *size, float **points)
{
    int index = 0;
    for (int i = 0; i < num_connections; ++i)
        if (true == g_clients[i].connected)
        {
            point_cloud[index * 2] = static_cast<float>(g_clients[i].x);
            point_cloud[index * 2 + 1] = static_cast<float>(g_clients[i].y);
            index++;
        }

    *size = index;
    *points = point_cloud;
}

#include <iostream>
#include <unordered_set>
#include <thread>
#include <vector>
#include <mutex>
#include <array>
#include <queue>z	
using namespace std;
#include <WS2tcpip.h>
#include <MSWSock.h>
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "MSWSock.lib")
#pragma comment(lib, "lua53.lib")

#include "..\..\2021_텀프_protocol.h"

;
extern "C" {
#include "LUA/lauxlib.h"
#include "LUA/lauxlib.h"
#include "LUA/lualib.h"
}

constexpr int ATTACK_RADIUS	= 1;

constexpr int MAX_BUFFER = 1024;

constexpr int SECTOR_X_SIZE = WORLD_WIDTH / 20;
constexpr int SECTOR_Y_SIZE = WORLD_HEIGHT / 20;

enum Input { Up, Down, Left, Right, Attack, None };
enum STAT { HP, LEVEL, EXP };

enum OP_TYPE {
	OP_RECV, OP_SEND, OP_ACCEPT, OP_SLEEP, OP_RANDOM_MOVE, OP_FLEE, OP_PLAYER_APPROACH,
	OP_RESPAWN
};

enum PLAYER_STATE { PLST_FREE, PLST_CONNECTED, PLST_INGAME };

enum CHAT_TYPE {
	CH_HIT_PLAYER, CH_HIT_MONSTER, CH_GET_EXP, CH_LEVEL_UP, CH_DEATH,
};


struct EX_OVER
{
	WSAOVERLAPPED	m_over;
	WSABUF			m_wsabuf[1];
	unsigned char	packetBuffer_[MAX_BUFFER];
	OP_TYPE			operaction_;
	SOCKET			m_csocket;					// OP_ACCEPT에서만 사용
};

struct TIMER_EVENT {
	int object;
	OP_TYPE e_type;
	chrono::system_clock::time_point start_time;
	int target_id;

	constexpr bool operator < (const TIMER_EVENT& L) const
	{
		return (start_time > L.start_time);
	}
};

mutex timer_l;
priority_queue <TIMER_EVENT> timer_queue;

struct S_OBJECT
{
	mutex				m_state_lock;
	atomic <PLAYER_STATE>	state_;
	SOCKET				m_socket;
	int					id;

	EX_OVER				m_recv_over;
	int					m_prev_size;
	atomic_bool			is_active;

	char				m_name[MAX_ID_LEN];
	int					cl, currentHp_, max_hp, level_, currentExp_, maxExp_, power_;
	short				x, y;
	int					sectorX, sectorY;
	int					move_time;
	
	unordered_set <int>	viewList_;
	mutex				viewListLock_;

	lua_State*			L;
	mutex				m_sl;

	unsigned char		move_stack;
	bool				sleep;
};

struct SECTOR
{
	unordered_set <int> objectList_;
	mutex s_mutex;
};

void disconnect(int p_id);

constexpr int SERVER_ID = 0;
array <S_OBJECT, MAX_USER + 1> objects;
SECTOR sector[SECTOR_X_SIZE][SECTOR_Y_SIZE];

HANDLE h_iocp;

const int ExpMultiplier = 2;
const int PowerIncrease = 10;


void add_event(int obj, OP_TYPE ev_t, int delay_ms)
{
	using namespace chrono;
	TIMER_EVENT ev;
	ev.e_type = ev_t;
	ev.object = obj;
	ev.start_time = system_clock::now() + milliseconds(delay_ms);
	timer_l.lock();
	timer_queue.push(ev);
	timer_l.unlock();
}

void wake_up_npc(int p_id, int npc_id)
{
	if (objects[npc_id].is_active == false) {
		bool old_state = false;
		if (true == atomic_compare_exchange_strong(&objects[npc_id].is_active,
			&old_state, true))
				add_event(npc_id, OP_RANDOM_MOVE, 1000);
			
	}

	//if (objects[p_id].x == objects[npc_id].x
	//	&& objects[p_id].y == objects[npc_id].y) {
	//}

}

bool isNpc(int id)
{
	return id > NPC_ID_START;
}

void displayError(const char* msg, int err_no)
{
	WCHAR* lpMsgBuf;
	FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
		NULL, err_no, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPTSTR)&lpMsgBuf, 0, NULL);
	cout << msg;
	wcout << lpMsgBuf << endl;
	LocalFree(lpMsgBuf);
}

bool canSee(int id_a, int id_b)
{
	return VIEW_RADIUS >= abs(objects[id_a].x - objects[id_b].x) &&
		VIEW_RADIUS >= abs(objects[id_a].y - objects[id_b].y);
}

bool canAttack(int id_a, int id_b)
{
	return ATTACK_RADIUS >= abs(objects[id_a].x - objects[id_b].x) &&
		ATTACK_RADIUS >= abs(objects[id_a].y - objects[id_b].y);
}

void doSend(int p_id, void *p)
{
	int p_size = reinterpret_cast<unsigned char*>(p)[0];
	int p_type = reinterpret_cast<unsigned char*>(p)[1];
	//cout << "To client [" << p_id << "] : ";
	//cout << "Packet [" << p_type << "]\n";
	EX_OVER* s_over = new EX_OVER;
	s_over->operaction_ = OP_SEND;
	memset(&s_over->m_over, 0, sizeof(s_over->m_over));
	memcpy(s_over->packetBuffer_, p, p_size);
	s_over->m_wsabuf[0].buf = reinterpret_cast<CHAR *>(s_over->packetBuffer_);
	s_over->m_wsabuf[0].len = p_size;
	int ret = WSASend(objects[p_id].m_socket, s_over->m_wsabuf, 1, 
		NULL, 0, &s_over->m_over, 0);
	if (0 != ret) {
		int err_no = WSAGetLastError();
		if (WSA_IO_PENDING != err_no) {
			displayError("WSASend : ", WSAGetLastError());
			disconnect(p_id);
		}
	}
}

void doRecv(int key)
{
	objects[key].m_recv_over.m_wsabuf[0].buf =
		reinterpret_cast<char *>(objects[key].m_recv_over.packetBuffer_)
		+ objects[key].m_prev_size;
	objects[key].m_recv_over.m_wsabuf[0].len = MAX_BUFFER - objects[key].m_prev_size;
	memset(&objects[key].m_recv_over.m_over, 0, sizeof(objects[key].m_recv_over.m_over));
	DWORD r_flag = 0;
	int ret = WSARecv(objects[key].m_socket, objects[key].m_recv_over.m_wsabuf, 1,
		NULL, &r_flag, &objects[key].m_recv_over.m_over, NULL);
	if (0 != ret) {
		int err_no = WSAGetLastError();
		if (WSA_IO_PENDING != err_no)
			displayError("WSARecv : ", WSAGetLastError());
	}
}

int getNewPlayerId(SOCKET p_socket)
{
	for (int i = SERVER_ID + 1; i <= MAX_USER; ++i) {
		lock_guard<mutex> lg { objects[i].m_state_lock };
		if (PLST_FREE == objects[i].state_) {
			objects[i].state_ = PLST_CONNECTED;
			objects[i].m_socket = p_socket;
			objects[i].m_name[0] = 0;
			return i;
		}
	}
	return -1;
}

void sendLoginOK(int p_id)
{
	sc_packet_login_ok p;
	p.HP = objects[p_id].currentHp_;
	p.id = p_id;
	p.LEVEL = objects[p_id].level_;
	p.EXP = objects[p_id].currentHp_;
	p.size = sizeof(p);
	p.type = SC_LOGIN_OK;
	p.x = objects[p_id].x;
	p.y = objects[p_id].y;
	doSend(p_id, &p);
}

void sendMoveObject(int c_id, int p_id)
{
	sc_packet_position p;
	p.id = p_id;
	p.size = sizeof(p);
	p.type = SC_POSITION;
	p.x = objects[p_id].x;
	p.y = objects[p_id].y;
	p.move_time = objects[p_id].move_time;
	doSend(c_id, &p);
}

void sendSpawnObject(int c_id, int p_id)
{
	sc_packet_add_object p;
	p.id = p_id;
	p.size = sizeof(p);
	p.type = SC_ADD_OBJECT;
	p.x = objects[p_id].x;
	p.y = objects[p_id].y;
	p.obj_class = 0; // 추후 변경 필요
	p.obj_class = objects[p_id].cl;
	p.HP = objects[p_id].currentHp_;
	p.LEVEL = objects[p_id].level_;
	p.EXP = objects[p_id].currentHp_;

	strcpy_s(p.name, objects[p_id].m_name);
	doSend(c_id, &p);
}

void sendRemoveObject(int c_id, int p_id)
{
	sc_packet_remove_object p;
	p.id = p_id;
	p.size = sizeof(p);
	p.type = SC_REMOVE_OBJECT;
	doSend(c_id, &p);
}

void send_chat(int c_id, int p_id, const char* mess)
{
	sc_packet_chat p;
	p.id = p_id;
	p.size = sizeof(p);
	p.type = SC_CHAT;
	strcpy_s(p.message, mess);
	doSend(c_id, &p);
}

void sendStatChange(int c_id, STAT stat, int num)
{
	sc_packet_stat_change p;
	p.id = c_id;
	p.size = sizeof(p);
	p.type = SC_STAT_CHANGE;

	p.HP = 0;
	p.LEVEL = 0;
	p.EXP = 0;

	switch (stat)
	{
	case HP:
		p.HP = num;
		break;
	case LEVEL:
		p.LEVEL = num;
		break;
	case EXP:
		p.EXP = num;
		break;
	}
	doSend(c_id, &p);
}



void do_move(int playerId, char dir)
{
	auto& x = objects[playerId].x;
	auto& y = objects[playerId].y;

	const int old_s_x = objects[playerId].sectorX;
	const int old_s_y = objects[playerId].sectorY;

	switch (dir) {
	case 0: if (y > 0) y--; break;
	case 1: if (y < (WORLD_HEIGHT - 1)) y++; break;
	case 2: if (x > 0) x--; break;
	case 3: if (x < (WORLD_WIDTH - 1)) x++; break;
	}

	const int newSectorX = x / SECTOR_X_SIZE;
	const int newSectorY = y / SECTOR_Y_SIZE;

	if (newSectorX == old_s_x && newSectorY == old_s_y) { 
		;
	}
	else {
		objects[playerId].sectorX = newSectorX;
		objects[playerId].sectorY = newSectorY;

		sector[old_s_x][old_s_y].s_mutex.lock();
		sector[old_s_x][old_s_y].objectList_.erase(playerId);
		sector[old_s_x][old_s_y].s_mutex.unlock();

		sector[newSectorX][newSectorY].s_mutex.lock();
		sector[newSectorX][newSectorY].objectList_.insert(playerId);
		sector[newSectorX][newSectorY].s_mutex.unlock();
	}


	objects[playerId].viewListLock_.lock();
	const unordered_set <int> oldViewList = objects[playerId].viewList_;
	objects[playerId].viewListLock_.unlock();
	
	unordered_set <int> newViewList;
	for (auto& id : sector[newSectorX][newSectorY].objectList_)
	{
		if (id == playerId) continue;

		if (objects[id].state_ == PLAYER_STATE::PLST_INGAME && canSee(id, playerId)) {
			newViewList.insert(id);

			if (true == isNpc(id)) {
				auto* ex_over = new EX_OVER;
				ex_over->operaction_ = OP_TYPE::OP_PLAYER_APPROACH;
				*reinterpret_cast<int*> (ex_over->packetBuffer_) = playerId;
				PostQueuedCompletionStatus(h_iocp, 1, id, &ex_over->m_over);
			}
		}
	}

	sendMoveObject(playerId, playerId);

	for (auto pl : newViewList) {	
		if (0 == oldViewList.count(pl)) { // 1. 새로 시야에 들어오는 오브젝트
			objects[playerId].viewListLock_.lock();
			objects[playerId].viewList_.insert(pl);
			objects[playerId].viewListLock_.unlock();
			sendSpawnObject(playerId, pl);

			if (false == isNpc(pl)) { // 플레이어인 경우
				objects[pl].viewListLock_.lock();
				if (0 == objects[pl].viewList_.count(playerId)) {
					objects[pl].viewList_.insert(playerId);
					objects[pl].viewListLock_.unlock();
					sendSpawnObject(pl, playerId); // 대상 플레이어한테도 알려줘야 함.
				}
				else {
					objects[pl].viewListLock_.unlock();
					sendMoveObject(pl, playerId); // 이미 알고있으면 움직였다고만 알려주면 됨.
				}
			}
			else { // npc인 경우
				objects[pl].sleep = false; // 안먹힘
				add_event(pl, OP_RANDOM_MOVE, 1000);
				wake_up_npc(playerId, pl); 
			}
			 // NPC 경우. 깨워야 함.
		}
		else {  // 2. 기존 시야에도 있고 새 시야에도 있는 경우
			if (false == isNpc(pl)) {
				objects[pl].viewListLock_.lock();
				if (0 == objects[pl].viewList_.count(playerId)) {
					objects[pl].viewList_.insert(playerId);
					objects[pl].viewListLock_.unlock();
					sendSpawnObject(pl, playerId);
				}
				else {
					objects[pl].viewListLock_.unlock();
					sendMoveObject(pl, playerId);
				}
			}
			else {
				objects[pl].sleep = false; // 안먹힘
			}
		}
	}



	for (auto pl : oldViewList) {
		if (0 == newViewList.count(pl)) {
			// 3. 시야에서 사라진 경우
			objects[playerId].viewListLock_.lock();
			objects[playerId].viewList_.erase(pl);
			objects[playerId].viewListLock_.unlock();
			sendRemoveObject(playerId, pl);

			if (false == isNpc(pl)) {
				objects[pl].viewListLock_.lock();
				if (0 != objects[pl].viewList_.count(playerId)) {
					objects[pl].viewList_.erase(playerId);
					objects[pl].viewListLock_.unlock();
					sendRemoveObject(pl, playerId);
				}
				else {
					objects[pl].viewListLock_.unlock();
				}
			}
			else {
				objects[pl].sleep = true;
			}
		}
	}

}

void process_packet(int currentPlayerId, unsigned char* packetBuffer)
{
	switch (packetBuffer[1]) {
	case CS_LOGIN: {
		cs_packet_login* packet = reinterpret_cast<cs_packet_login*>(packetBuffer);
		lock_guard <mutex> gl2{ objects[currentPlayerId].m_state_lock };
		strcpy_s(objects[currentPlayerId].m_name, packet->player_id);
		objects[currentPlayerId].x = rand() % WORLD_WIDTH;
		objects[currentPlayerId].y = rand() % WORLD_HEIGHT;

		objects[currentPlayerId].sectorX = objects[currentPlayerId].x / SECTOR_X_SIZE;
		objects[currentPlayerId].sectorY = objects[currentPlayerId].y / SECTOR_Y_SIZE;

		sector[objects[currentPlayerId].sectorX][objects[currentPlayerId].sectorY].s_mutex.lock();
		sector[objects[currentPlayerId].sectorX][objects[currentPlayerId].sectorY].objectList_.insert(currentPlayerId);
		sector[objects[currentPlayerId].sectorX][objects[currentPlayerId].sectorY].s_mutex.unlock();

		sendLoginOK(currentPlayerId);
		objects[currentPlayerId].state_ = PLST_INGAME;

		for (auto& pl : sector[objects[currentPlayerId].sectorX][objects[currentPlayerId].sectorY].objectList_) {
			if (currentPlayerId != pl) {
				lock_guard <mutex> gl{ objects[pl].m_state_lock };
				if (PLST_INGAME == objects[pl].state_) {
					if (canSee(currentPlayerId, pl)) {
						objects[currentPlayerId].viewListLock_.lock();
						objects[currentPlayerId].viewList_.insert(pl);
						objects[currentPlayerId].viewListLock_.unlock();
						sendSpawnObject(currentPlayerId, pl);
						if (false == isNpc(pl)) {
							objects[pl].viewListLock_.lock();
							objects[pl].viewList_.insert(currentPlayerId);
							objects[pl].viewListLock_.unlock();
							sendSpawnObject(pl, currentPlayerId);
						}
						else {
							wake_up_npc(currentPlayerId, pl);
						}
					}
				}
			}
		}
				 break;
	}
	case CS_MOVE: {
		cs_packet_move* packet = reinterpret_cast<cs_packet_move*>(packetBuffer);
		objects[currentPlayerId].move_time = packet->move_time;
		do_move(currentPlayerId, packet->direction);
		break;
	}
	case CS_ATTACK: {
		cs_packet_attack* packet = reinterpret_cast<cs_packet_attack*>(packetBuffer);
		for (auto& playerId : sector[objects[currentPlayerId].sectorX][objects[currentPlayerId].sectorY].objectList_) {
			if (currentPlayerId != playerId) {
				// 플레이어가 공격하면 데미지 입어야함.
				if (PLST_INGAME == objects[playerId].state_) {
					if (canAttack(currentPlayerId, playerId) && objects[playerId].currentHp_ > 0) {
						objects[playerId].currentHp_ -= objects[currentPlayerId].power_;

						// 입은 데미지 전송
						sendStatChange(currentPlayerId, STAT::HP, objects[playerId].power_);

						// 입힌 데미지 전송
						sendStatChange(currentPlayerId, STAT::HP, objects[currentPlayerId].power_);

						int exp = 0;

						// 체력이 0 이하로 떨어지면 섹터를 업데이트 하고 리스폰 처리를 함.
						
						if (objects[playerId].currentHp_ <= 0) {
							exp = objects[currentPlayerId].level_ * objects[currentPlayerId].level_ * 2;
							sendStatChange(currentPlayerId, STAT::EXP, exp);
							sendRemoveObject(currentPlayerId, playerId);
							add_event(playerId, OP_TYPE::OP_RESPAWN, 30000);
						}

						objects[playerId].currentExp_ += exp;

						while (objects[playerId].currentExp_ > objects[playerId].maxExp_) {
							objects[playerId].currentExp_ -= objects[playerId].maxExp_;
							objects[playerId].level_++;
							objects[playerId].maxExp_ *= ExpMultiplier;
							objects[playerId].power_ += PowerIncrease;
							sendStatChange(currentPlayerId, STAT::LEVEL, objects[playerId].level_);
						}
					}
				}
			}
		}
				  break;
	}
	case CS_LOGOUT: {
		disconnect(currentPlayerId);

		for (auto& pl : sector[objects[currentPlayerId].sectorX][objects[currentPlayerId].sectorY].objectList_) {
			if (canSee(pl, currentPlayerId) && false == isNpc(pl)) {
				sendRemoveObject(pl, currentPlayerId);
			}

		}

		break;
	}
	case CS_TELEPORT: {
		cs_packet_teleport* packet = reinterpret_cast<cs_packet_teleport*>(packetBuffer);
		objects[currentPlayerId].x = rand() % WORLD_WIDTH;
		objects[currentPlayerId].y = rand() % WORLD_HEIGHT;

		int new_s_x = objects[currentPlayerId].x / WORLD_WIDTH;
		int new_s_y = objects[currentPlayerId].y / WORLD_HEIGHT;

		for (auto& pl : sector[objects[currentPlayerId].sectorX][objects[currentPlayerId].sectorY].objectList_) {
			if (canSee(currentPlayerId, pl) && false == isNpc(currentPlayerId)) {
				sendRemoveObject(pl, currentPlayerId);
			}
		}

		sector[objects[currentPlayerId].sectorX][objects[currentPlayerId].sectorY].s_mutex.lock();
		sector[objects[currentPlayerId].sectorX][objects[currentPlayerId].sectorY].objectList_.erase(currentPlayerId);
		sector[objects[currentPlayerId].sectorX][objects[currentPlayerId].sectorY].s_mutex.unlock();

		objects[currentPlayerId].sectorX = new_s_x;
		objects[currentPlayerId].sectorY = new_s_y;

		sector[objects[currentPlayerId].sectorX][objects[currentPlayerId].sectorY].s_mutex.lock();
		sector[objects[currentPlayerId].sectorX][objects[currentPlayerId].sectorY].objectList_.insert(currentPlayerId);
		sector[objects[currentPlayerId].sectorX][objects[currentPlayerId].sectorY].s_mutex.unlock();

		for (auto& pl : sector[objects[currentPlayerId].sectorX][objects[currentPlayerId].sectorY].objectList_) {
			if (canSee(currentPlayerId, pl) && false == isNpc(currentPlayerId)) {
				sendSpawnObject(pl, currentPlayerId);
			}
		}

		sendMoveObject(currentPlayerId, currentPlayerId);

		break;
	}

	default:
		cout << "Unknown Packet Type from Client[" << currentPlayerId;
		cout << "] Packet Type [" << packetBuffer[1] << "]";
		while (true);
		break;
	}
}

void disconnect(int p_id)
{
	{
		lock_guard <mutex> gl{ objects[p_id].m_state_lock };
		if (objects[p_id].state_ = PLST_FREE) return;
		closesocket(objects[p_id].m_socket);	
		objects[p_id].state_ = PLST_FREE;
	}
	for (auto& pl : objects) {
		if (false == isNpc(pl.id)) {
			lock_guard<mutex> gl2{ pl.m_state_lock };
			if (PLST_INGAME == pl.state_)
				sendRemoveObject(pl.id, p_id);
		}
	}
}

void do_npc_random_move(S_OBJECT& npc)
{
	unordered_set<int> old_vl;
	for (auto& o_id : sector[npc.sectorX][npc.sectorY].objectList_) {
		if (PLST_INGAME != objects[o_id].state_) continue;
		if (true == isNpc(o_id)) continue;
		if (true == canSee(npc.id, o_id))
			old_vl.insert(o_id);
	}

	int x = npc.x;
	int y = npc.y;
	int old_s_x = npc.sectorX;
	int old_s_y = npc.sectorY;

	switch (rand() % 4) {
	case 0: if (x < (WORLD_WIDTH - 1)) x++; break;
	case 1: if (x > 0) x--; break;
	case 2: if (y < (WORLD_HEIGHT - 1)) y++; break;
	case 3: if (y > 0) y--; break;
	}

	npc.x = x;
	npc.y = y;

	int new_s_x = x / SECTOR_X_SIZE;
	int new_s_y = y / SECTOR_Y_SIZE;
	
	if (new_s_x == npc.sectorX && new_s_y == npc.sectorY) {
	}
	else {
		npc.sectorX = new_s_x;
		npc.sectorY = new_s_y;

		sector[old_s_x][old_s_y].s_mutex.lock();
		sector[old_s_x][old_s_y].objectList_.erase(npc.id);
		sector[old_s_x][old_s_y].s_mutex.unlock();

		sector[new_s_x][new_s_y].s_mutex.lock();
		sector[new_s_x][new_s_y].objectList_.insert(npc.id);
		sector[new_s_x][new_s_y].s_mutex.unlock();
	}

	unordered_set<int> new_vl;
	for (auto& o_id : sector[npc.sectorX][npc.sectorY].objectList_) {
		if (PLST_INGAME != objects[o_id].state_)  continue;
		if (true == isNpc(o_id)) continue;
		
		if (true == canSee(npc.id, o_id)) {
			new_vl.insert(o_id);
		}
	}

	for (auto pl : new_vl) {
		if (0 == old_vl.count(pl)) {
			// 플레이어의 시야에 등장
			objects[pl].viewListLock_.lock();
			objects[pl].viewList_.insert(npc.id);
			objects[pl].viewListLock_.unlock();
			sendSpawnObject(pl, npc.id);
		}
		else {
			// 플레이어가 계속 보고 있음.
			sendMoveObject(pl, npc.id);
		}
	}

	for (auto pl : old_vl) {
		if (0 == new_vl.count(pl)) {
			objects[pl].viewListLock_.lock();
			if (0 != objects[pl].viewList_.count(npc.id)) {
				objects[pl].viewList_.erase(npc.id);
				objects[pl].viewListLock_.unlock();
				objects[npc.id].sleep = true;
				sendRemoveObject(pl, npc.id);
			}
			else {
				objects[pl].viewListLock_.unlock();
			}
		}
	}
}

void worker(HANDLE h_iocp, SOCKET l_socket)
{
	while (true) {
		DWORD num_bytes;
		ULONG_PTR ikey;
		WSAOVERLAPPED* over;

		BOOL ret = GetQueuedCompletionStatus(h_iocp, &num_bytes,
			&ikey, &over, INFINITE);

		int key = static_cast<int>(ikey);
		if (FALSE == ret) {
			if (SERVER_ID == key) {
				displayError("GQCS : ", WSAGetLastError());
				exit(-1);
			}
			else {
				displayError("GQCS : ", WSAGetLastError());
				disconnect(key);
			}
		}
		if ((key != SERVER_ID) && (0 == num_bytes)) {
			disconnect(key);
			continue;
		}
		EX_OVER* ex_over = reinterpret_cast<EX_OVER*>(over);

		switch (ex_over->operaction_) {
		case OP_RECV: {
			unsigned char* packet_ptr = ex_over->packetBuffer_;
			int num_data = num_bytes + objects[key].m_prev_size;
			int packet_size = packet_ptr[0];

			while (num_data >= packet_size) {
				process_packet(key, packet_ptr);
				num_data -= packet_size;
				packet_ptr += packet_size;
				if (0 >= num_data) break;
				packet_size = packet_ptr[0];
			}
			objects[key].m_prev_size = num_data;
			if (0 != num_data)
				memcpy(ex_over->packetBuffer_, packet_ptr, num_data);
			doRecv(key);
		}
					break;
		case OP_SEND:
			delete ex_over;
			break;
		case OP_ACCEPT:
		{
			int c_id = getNewPlayerId(ex_over->m_csocket);
			if (-1 != c_id) {
				objects[c_id].m_recv_over.operaction_ = OP_RECV;
				objects[c_id].m_prev_size = 0;
				CreateIoCompletionPort(
					reinterpret_cast<HANDLE>(objects[c_id].m_socket), h_iocp, c_id, 0);
				doRecv(c_id);
			}
			else {
				closesocket(objects[c_id].m_socket);
			}

			memset(&ex_over->m_over, 0, sizeof(ex_over->m_over));
			SOCKET c_socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
			ex_over->m_csocket = c_socket;
			AcceptEx(l_socket, c_socket,
				ex_over->packetBuffer_, 0, 32, 32, NULL, &ex_over->m_over);
		}
		break;
		case OP_SLEEP:
			add_event(key, OP_FLEE, 1000);
			delete ex_over;
			break;
		case OP_RANDOM_MOVE:
			if (false == objects[key].sleep) {
				do_npc_random_move(objects[key]);
				add_event(key, OP_RANDOM_MOVE, 1000);
			}
			delete ex_over;
			break;
		case OP_FLEE: {
			//objects[key].m_sl.lock();
			//int move_player = *reinterpret_cast<int*>(ex_over->m_packetbuf);
			//lua_State* L = objects[key].L;
			//lua_getglobal(L, "player_is_near");
			//lua_pushnumber(L, move_player);
			//lua_pcall(L, 1, 0, 0);
			//objects[key].m_sl.unlock();
			//delete ex_over;
			break;
		}
		case OP_PLAYER_APPROACH: {
			objects[key].m_sl.lock();
			int move_player = *reinterpret_cast<int*>(ex_over->packetBuffer_);
			lua_State* L = objects[key].L;
			lua_getglobal(L, "player_is_near");
			lua_pushnumber(L, move_player);
			lua_pcall(L, 1, 0, 0);
			objects[key].m_sl.unlock();
			delete ex_over;
			break;
		}
		case OP_RESPAWN: {
			add_event(key, OP_RANDOM_MOVE, 5000);
			break;
		}
		}
	}
}

void do_ai()
{
	using namespace chrono;

	for (;;) {
		auto start_t = chrono::system_clock::now();
		for (auto& npc : objects) {
			if (true == isNpc(npc.id)) {
				do_npc_random_move(npc);
			}
		}
		auto end_t = chrono::system_clock::now();
		auto ai_time = end_t - start_t;
		cout << "AI Exec Time : "
			<< duration_cast<milliseconds>(ai_time).count()
			<< "ms.\n";
		if (end_t < start_t + 1s)
			this_thread::sleep_for(start_t + seconds(1) - end_t);
	}
}

void do_timer()
{
	using namespace chrono;

	for(;;) {
		timer_l.lock();
		if ((false == timer_queue.empty())
			&& (timer_queue.top().start_time <= system_clock::now())) {
			TIMER_EVENT ev = timer_queue.top();
			timer_queue.pop();
			timer_l.unlock();
			EX_OVER* ex_over = new EX_OVER;
			ex_over->operaction_ = OP_RANDOM_MOVE;
			PostQueuedCompletionStatus(h_iocp, 1, ev.object, &ex_over->m_over);
		}
		else {
			timer_l.unlock();
			this_thread::sleep_for(10ms);
		}
	}
}

int API_get_x(lua_State* L)
{
	int obj_id = lua_tonumber(L, -1);
	lua_pop(L, 2);
	int x = objects[obj_id].x;
	lua_pushnumber(L, x);

	return 1;
}
int API_get_y(lua_State* L)
{
	int obj_id = lua_tonumber(L, -1);
	lua_pop(L, 2);
	int y = objects[obj_id].y;
	lua_pushnumber(L, y);

	return 1;

}
int API_send_mess(lua_State* L)
{
	int p_id = lua_tonumber(L, -3);
	int o_id = lua_tonumber(L, -2);
	const char* mess = lua_tostring(L, -1);
	lua_pop(L, 4);
	send_chat(p_id, o_id, mess);
	return 0;
}
int API_move_rand(lua_State* L)
{
	int obj_id = lua_tonumber(L, -1);
	lua_pop(L, 2);
	int stack = objects[obj_id].move_stack;

	if (stack <= 3)
		lua_pushnumber(L, stack);

	return 1;
}


int main()
{
	for (int i = 0; i < MAX_USER + 1; ++i) {
		auto& pl = objects[i];
		pl.id = i;
		pl.state_ = PLST_FREE;
		pl.sleep = false;
		pl.level_ = 1;
		pl.max_hp = 100;
		pl.currentHp_ = 100;
		pl.maxExp_ = 100;
		pl.currentExp_ = 0;
		pl.power_ = 10;

		if (true == isNpc(i)) {
			sprintf_s(pl.m_name, "N%d", i);
			pl.state_ = PLST_INGAME;
			pl.x = rand() % WORLD_WIDTH;
			pl.y = rand() % WORLD_HEIGHT;
			pl.sectorX = pl.x / SECTOR_X_SIZE;
			pl.sectorY = pl.y / SECTOR_Y_SIZE;
			sector[pl.sectorX][pl.sectorY].objectList_.insert(pl.id);

			pl.move_stack = 0;
			
			//
			lua_State* L = pl.L = luaL_newstate();
			luaL_openlibs(L);
			luaL_loadfile(L, "npc.lua");
			int res = lua_pcall(L, 0, 0, 0);
			if (0 != res) {
				cout << "LUA error in exec: " << lua_tostring(L, -1) << endl;
			}

			lua_getglobal(L, "set_object_id");
			lua_pushnumber(L, i);
			lua_pcall(L, 1, 0, 0);

			lua_register(L, "API_get_x", API_get_x);
			lua_register(L, "API_get_y", API_get_y);
			lua_register(L, "API_send_mess", API_send_mess);
		}
	}
		cout << "Init done" << endl;


	WSADATA WSAData;
	WSAStartup(MAKEWORD(2, 2), &WSAData);

	wcout.imbue(locale("korean"));
	h_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, 0, 0, 0);
	SOCKET listenSocket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	CreateIoCompletionPort(reinterpret_cast<HANDLE>(listenSocket), h_iocp, SERVER_ID, 0);
	SOCKADDR_IN serverAddr;
	memset(&serverAddr, 0, sizeof(SOCKADDR_IN));
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons(SERVER_PORT);
	serverAddr.sin_addr.S_un.S_addr = htonl(INADDR_ANY);
	::bind(listenSocket, (struct sockaddr*)&serverAddr, sizeof(SOCKADDR_IN));
	listen(listenSocket, SOMAXCONN);

	EX_OVER accept_over;
	accept_over.operaction_ = OP_ACCEPT;
	memset(&accept_over.m_over, 0, sizeof(accept_over.m_over));
	SOCKET c_socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	accept_over.m_csocket = c_socket;
	BOOL ret = AcceptEx(listenSocket, c_socket,
		accept_over.packetBuffer_, 0, 32, 32, NULL, &accept_over.m_over);
	if (FALSE == ret) {
		int err_num = WSAGetLastError();
		if (err_num != WSA_IO_PENDING)
			displayError("AcceptEx Error", err_num);
	}

	vector <thread> worker_threads;
	for (int i = 0; i < 4; ++i)
		worker_threads.emplace_back(worker, h_iocp, listenSocket);

	//thread ai_thread{ do_ai };
	//ai_thread.join();

	thread timer_thread{ do_timer };
	timer_thread.join();

	for (auto& th : worker_threads)
		th.join();

	closesocket(listenSocket);
	WSACleanup();
}

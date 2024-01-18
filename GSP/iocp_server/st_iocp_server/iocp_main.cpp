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

enum PL_STATE { PLST_FREE, PLST_CONNECTED, PLST_INGAME };

enum CHAT_TYPE {
	CH_HIT_PLAYER, CH_HIT_MONSTER, CH_GET_EXP, CH_LEVEL_UP, CH_DEATH,
};


struct EX_OVER
{
	WSAOVERLAPPED	m_over;
	WSABUF			m_wsabuf[1];
	unsigned char	m_packetbuf[MAX_BUFFER];
	OP_TYPE			m_op;
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
	mutex				m_slock;
	atomic <PL_STATE>	m_state;
	SOCKET				m_socket;
	int					id;

	EX_OVER				m_recv_over;
	int					m_prev_size;
	atomic_bool			is_active;

	char				m_name[MAX_ID_LEN];
	int					cl, cur_hp, max_hp, level, cur_exp, max_exp, power;
	short				x, y;
	int					s_x, s_y;
	int					move_time;
	
	unordered_set <int>	m_view_list;
	mutex				m_vl;

	lua_State*			L;
	mutex				m_sl;

	unsigned char		move_stack;
	bool				sleep;
};

struct SECTOR
{
	unordered_set <int> object_list;
	mutex s_mutex;
};

void disconnect(int p_id);

constexpr int SERVER_ID = 0;
array <S_OBJECT, MAX_USER + 1> objects;
SECTOR sector[SECTOR_X_SIZE][SECTOR_Y_SIZE];

HANDLE h_iocp;



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

bool is_npc(int id)
{
	return id > NPC_ID_START;
}

void display_error(const char* msg, int err_no)
{
	WCHAR* lpMsgBuf;
	FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
		NULL, err_no, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPTSTR)&lpMsgBuf, 0, NULL);
	cout << msg;
	wcout << lpMsgBuf << endl;
	LocalFree(lpMsgBuf);
}

bool can_see(int id_a, int id_b)
{
	return VIEW_RADIUS >= abs(objects[id_a].x - objects[id_b].x) &&
		VIEW_RADIUS >= abs(objects[id_a].y - objects[id_b].y);
}

bool can_attack(int id_a, int id_b)
{
	return ATTACK_RADIUS >= abs(objects[id_a].x - objects[id_b].x) &&
		ATTACK_RADIUS >= abs(objects[id_a].y - objects[id_b].y);
}

void send_packet(int p_id, void *p)
{
	int p_size = reinterpret_cast<unsigned char*>(p)[0];
	int p_type = reinterpret_cast<unsigned char*>(p)[1];
	//cout << "To client [" << p_id << "] : ";
	//cout << "Packet [" << p_type << "]\n";
	EX_OVER* s_over = new EX_OVER;
	s_over->m_op = OP_SEND;
	memset(&s_over->m_over, 0, sizeof(s_over->m_over));
	memcpy(s_over->m_packetbuf, p, p_size);
	s_over->m_wsabuf[0].buf = reinterpret_cast<CHAR *>(s_over->m_packetbuf);
	s_over->m_wsabuf[0].len = p_size;
	int ret = WSASend(objects[p_id].m_socket, s_over->m_wsabuf, 1, 
		NULL, 0, &s_over->m_over, 0);
	if (0 != ret) {
		int err_no = WSAGetLastError();
		if (WSA_IO_PENDING != err_no) {
			display_error("WSASend : ", WSAGetLastError());
			disconnect(p_id);
		}
	}
}

void do_recv(int key)
{
	objects[key].m_recv_over.m_wsabuf[0].buf =
		reinterpret_cast<char *>(objects[key].m_recv_over.m_packetbuf)
		+ objects[key].m_prev_size;
	objects[key].m_recv_over.m_wsabuf[0].len = MAX_BUFFER - objects[key].m_prev_size;
	memset(&objects[key].m_recv_over.m_over, 0, sizeof(objects[key].m_recv_over.m_over));
	DWORD r_flag = 0;
	int ret = WSARecv(objects[key].m_socket, objects[key].m_recv_over.m_wsabuf, 1,
		NULL, &r_flag, &objects[key].m_recv_over.m_over, NULL);
	if (0 != ret) {
		int err_no = WSAGetLastError();
		if (WSA_IO_PENDING != err_no)
			display_error("WSARecv : ", WSAGetLastError());
	}
}

int get_new_player_id(SOCKET p_socket)
{
	for (int i = SERVER_ID + 1; i <= MAX_USER; ++i) {
		lock_guard<mutex> lg { objects[i].m_slock };
		if (PLST_FREE == objects[i].m_state) {
			objects[i].m_state = PLST_CONNECTED;
			objects[i].m_socket = p_socket;
			objects[i].m_name[0] = 0;
			return i;
		}
	}
	return -1;
}

void send_login_ok_packet(int p_id)
{
	sc_packet_login_ok p;
	p.HP = objects[p_id].cur_hp;
	p.id = p_id;
	p.LEVEL = objects[p_id].level;
	p.EXP = objects[p_id].cur_hp;
	p.size = sizeof(p);
	p.type = SC_LOGIN_OK;
	p.x = objects[p_id].x;
	p.y = objects[p_id].y;
	send_packet(p_id, &p);
}

void send_move_packet(int c_id, int p_id)
{
	sc_packet_position p;
	p.id = p_id;
	p.size = sizeof(p);
	p.type = SC_POSITION;
	p.x = objects[p_id].x;
	p.y = objects[p_id].y;
	p.move_time = objects[p_id].move_time;
	send_packet(c_id, &p);
}

void send_add_object(int c_id, int p_id)
{
	sc_packet_add_object p;
	p.id = p_id;
	p.size = sizeof(p);
	p.type = SC_ADD_OBJECT;
	p.x = objects[p_id].x;
	p.y = objects[p_id].y;
	p.obj_class = 0; // 추후 변경 필요
	p.obj_class = objects[p_id].cl;
	p.HP = objects[p_id].cur_hp;
	p.LEVEL = objects[p_id].level;
	p.EXP = objects[p_id].cur_hp;

	strcpy_s(p.name, objects[p_id].m_name);
	send_packet(c_id, &p);
}

void send_remove_object(int c_id, int p_id)
{
	sc_packet_remove_object p;
	p.id = p_id;
	p.size = sizeof(p);
	p.type = SC_REMOVE_OBJECT;
	send_packet(c_id, &p);
}

void send_chat(int c_id, int p_id, const char* mess)
{
	sc_packet_chat p;
	p.id = p_id;
	p.size = sizeof(p);
	p.type = SC_CHAT;
	strcpy_s(p.message, mess);
	send_packet(c_id, &p);
}

void send_stat_change(int c_id, STAT stat, int num)
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
	send_packet(c_id, &p);
}



void do_move(int p_id, char dir)
{
	auto& x = objects[p_id].x;
	auto& y = objects[p_id].y;

	int old_s_x = objects[p_id].s_x;
	int old_s_y = objects[p_id].s_y;

	switch (dir) {
	case 0: if (y > 0) y--; break;
	case 1: if (y < (WORLD_HEIGHT - 1)) y++; break;
	case 2: if (x > 0) x--; break;
	case 3: if (x < (WORLD_WIDTH - 1)) x++; break;
	}

	int new_s_x = x / SECTOR_X_SIZE;
	int new_s_y = y / SECTOR_Y_SIZE;

	if (new_s_x == old_s_x && new_s_y == old_s_y) { 
		;
	}
	else {
		objects[p_id].s_x = new_s_x;
		objects[p_id].s_y = new_s_y;

		sector[old_s_x][old_s_y].s_mutex.lock();
		sector[old_s_x][old_s_y].object_list.erase(p_id);
		sector[old_s_x][old_s_y].s_mutex.unlock();

		sector[new_s_x][new_s_y].s_mutex.lock();
		sector[new_s_x][new_s_y].object_list.insert(p_id);
		sector[new_s_x][new_s_y].s_mutex.unlock();
	}

	unordered_set <int> old_vl;

	objects[p_id].m_vl.lock();
	old_vl = objects[p_id].m_view_list;
	objects[p_id].m_vl.unlock();
	
	unordered_set <int> new_vl;
	for (auto& pl : sector[new_s_x][new_s_y].object_list)
	{
		if (pl == p_id) continue;

		if (objects[pl].m_state == PLST_INGAME && can_see(pl, p_id)) {
			new_vl.insert(pl);

			if (true == is_npc(pl)) {
				EX_OVER* ex_over = new EX_OVER;
				ex_over->m_op = OP_PLAYER_APPROACH;
				*reinterpret_cast<int*> (ex_over->m_packetbuf) = p_id;
				PostQueuedCompletionStatus(h_iocp, 1, pl, &ex_over->m_over);
			}
		}
	}

	send_move_packet(p_id, p_id);

	for (auto pl : new_vl) {	
		if (0 == old_vl.count(pl)) { // 1. 새로 시야에 들어오는 오브젝트
			objects[p_id].m_vl.lock();
			objects[p_id].m_view_list.insert(pl);
			objects[p_id].m_vl.unlock();
			send_add_object(p_id, pl);

			if (false == is_npc(pl)) { // 플레이어인 경우
				objects[pl].m_vl.lock();
				if (0 == objects[pl].m_view_list.count(p_id)) {
					objects[pl].m_view_list.insert(p_id);
					objects[pl].m_vl.unlock();
					send_add_object(pl, p_id); // 대상 플레이어한테도 알려줘야 함.
				}
				else {
					objects[pl].m_vl.unlock();
					send_move_packet(pl, p_id); // 이미 알고있으면 움직였다고만 알려주면 됨.
				}
			}
			else { // npc인 경우
				objects[pl].sleep = false; // 안먹힘
				add_event(pl, OP_RANDOM_MOVE, 1000);
				wake_up_npc(p_id, pl); 
			}
			 // NPC 경우. 깨워야 함.
		}
		else {  // 2. 기존 시야에도 있고 새 시야에도 있는 경우
			if (false == is_npc(pl)) {
				objects[pl].m_vl.lock();
				if (0 == objects[pl].m_view_list.count(p_id)) {
					objects[pl].m_view_list.insert(p_id);
					objects[pl].m_vl.unlock();
					send_add_object(pl, p_id);
				}
				else {
					objects[pl].m_vl.unlock();
					send_move_packet(pl, p_id);
				}
			}
			else {
				objects[pl].sleep = false; // 안먹힘
			}
		}
	}



	for (auto pl : old_vl) {
		if (0 == new_vl.count(pl)) {
			// 3. 시야에서 사라진 경우
			objects[p_id].m_vl.lock();
			objects[p_id].m_view_list.erase(pl);
			objects[p_id].m_vl.unlock();
			send_remove_object(p_id, pl);

			if (false == is_npc(pl)) {
				objects[pl].m_vl.lock();
				if (0 != objects[pl].m_view_list.count(p_id)) {
					objects[pl].m_view_list.erase(p_id);
					objects[pl].m_vl.unlock();
					send_remove_object(pl, p_id);
				}
				else {
					objects[pl].m_vl.unlock();
				}
			}
			else {
				objects[pl].sleep = true;
			}
		}
	}

}

void process_packet(int p_id, unsigned char* p_buf)
{
	switch (p_buf[1]) {
	case CS_LOGIN: {
		cs_packet_login* packet = reinterpret_cast<cs_packet_login*>(p_buf);
		lock_guard <mutex> gl2{ objects[p_id].m_slock };
		strcpy_s(objects[p_id].m_name, packet->player_id);
		objects[p_id].x = rand() % WORLD_WIDTH;
		objects[p_id].y = rand() % WORLD_HEIGHT;

		objects[p_id].s_x = objects[p_id].x / SECTOR_X_SIZE;
		objects[p_id].s_y = objects[p_id].y / SECTOR_Y_SIZE;

		sector[objects[p_id].s_x][objects[p_id].s_y].s_mutex.lock();
		sector[objects[p_id].s_x][objects[p_id].s_y].object_list.insert(p_id);
		sector[objects[p_id].s_x][objects[p_id].s_y].s_mutex.unlock();

		send_login_ok_packet(p_id);
		objects[p_id].m_state = PLST_INGAME;

		for (auto& pl : sector[objects[p_id].s_x][objects[p_id].s_y].object_list) {
			if (p_id != pl) {
				lock_guard <mutex> gl{ objects[pl].m_slock };
				if (PLST_INGAME == objects[pl].m_state) {
					if (can_see(p_id, pl)) {
						objects[p_id].m_vl.lock();
						objects[p_id].m_view_list.insert(pl);
						objects[p_id].m_vl.unlock();
						send_add_object(p_id, pl);
						if (false == is_npc(pl)) {
							objects[pl].m_vl.lock();
							objects[pl].m_view_list.insert(p_id);
							objects[pl].m_vl.unlock();
							send_add_object(pl, p_id);
						}
						else {
							wake_up_npc(p_id, pl);
						}
					}
				}
			}
		}
				 break;
	}
	case CS_MOVE: {
		cs_packet_move* packet = reinterpret_cast<cs_packet_move*>(p_buf);
		objects[p_id].move_time = packet->move_time;
		do_move(p_id, packet->direction);
		break;
	}
	case CS_ATTACK: {
		cs_packet_attack* packet = reinterpret_cast<cs_packet_attack*>(p_buf);
		for (auto& pl : sector[objects[p_id].s_x][objects[p_id].s_y].object_list) {
			if (p_id != pl) {
				// 플레이어가 공격하면 데미지 입어야함.
				if (PLST_INGAME == objects[pl].m_state) {
					if (can_attack(p_id, pl) && objects[pl].cur_hp > 0) {
						objects[pl].cur_hp -= objects[p_id].power;
						send_stat_change(p_id, HP, objects[pl].power);
						// 입힌 데미지 전송.
						send_stat_change(p_id, HP, objects[p_id].power);

						int exp = 0;

						// 체력이 0 이하로 떨어지면 섹터를 업데이트 하고 리스폰 처리를 함.
						
						if (objects[pl].cur_hp <= 0) {
							exp = objects[p_id].level * objects[p_id].level * 2;
							send_stat_change(p_id, EXP, exp);
							send_remove_object(p_id, pl);
							add_event(pl, OP_RESPAWN, 30000);
						}

						objects[pl].cur_exp += exp;

						while (objects[pl].cur_exp > objects[pl].max_exp) {
							objects[pl].cur_exp -= objects[pl].max_exp;
							objects[pl].level++;
							objects[pl].max_exp *= 2;
							objects[pl].power += 10;
							send_stat_change(p_id, LEVEL, objects[pl].level);
						}
					}
				}
			}
		}
				  break;
	}
	case CS_LOGOUT: {
		disconnect(p_id);

		for (auto& pl : sector[objects[p_id].s_x][objects[p_id].s_y].object_list) {
			if (can_see(pl, p_id) && false == is_npc(pl)) {
				send_remove_object(pl, p_id);
			}

		}

		break;
	}
	case CS_TELEPORT: {
		cs_packet_teleport* packet = reinterpret_cast<cs_packet_teleport*>(p_buf);
		objects[p_id].x = rand() % WORLD_WIDTH;
		objects[p_id].y = rand() % WORLD_HEIGHT;

		int new_s_x = objects[p_id].x / WORLD_WIDTH;
		int new_s_y = objects[p_id].y / WORLD_HEIGHT;

		for (auto& pl : sector[objects[p_id].s_x][objects[p_id].s_y].object_list) {
			if (can_see(p_id, pl) && false == is_npc(p_id)) {
				send_remove_object(pl, p_id);
			}
		}

		sector[objects[p_id].s_x][objects[p_id].s_y].s_mutex.lock();
		sector[objects[p_id].s_x][objects[p_id].s_y].object_list.erase(p_id);
		sector[objects[p_id].s_x][objects[p_id].s_y].s_mutex.unlock();

		objects[p_id].s_x = new_s_x;
		objects[p_id].s_y = new_s_y;

		sector[objects[p_id].s_x][objects[p_id].s_y].s_mutex.lock();
		sector[objects[p_id].s_x][objects[p_id].s_y].object_list.insert(p_id);
		sector[objects[p_id].s_x][objects[p_id].s_y].s_mutex.unlock();

		for (auto& pl : sector[objects[p_id].s_x][objects[p_id].s_y].object_list) {
			if (can_see(p_id, pl) && false == is_npc(p_id)) {
				send_add_object(pl, p_id);
			}
		}

		send_move_packet(p_id, p_id);

		break;
	}

	default:
		cout << "Unknown Packet Type from Client[" << p_id;
		cout << "] Packet Type [" << p_buf[1] << "]";
		while (true);
		break;
	}
}

void disconnect(int p_id)
{
	{
		lock_guard <mutex> gl{ objects[p_id].m_slock };
		if (objects[p_id].m_state = PLST_FREE) return;
		closesocket(objects[p_id].m_socket);	
		objects[p_id].m_state = PLST_FREE;
	}
	for (auto& pl : objects) {
		if (false == is_npc(pl.id)) {
			lock_guard<mutex> gl2{ pl.m_slock };
			if (PLST_INGAME == pl.m_state)
				send_remove_object(pl.id, p_id);
		}
	}
}

void do_npc_random_move(S_OBJECT& npc)
{
	unordered_set<int> old_vl;
	for (auto& o_id : sector[npc.s_x][npc.s_y].object_list) {
		if (PLST_INGAME != objects[o_id].m_state) continue;
		if (true == is_npc(o_id)) continue;
		if (true == can_see(npc.id, o_id))
			old_vl.insert(o_id);
	}

	int x = npc.x;
	int y = npc.y;
	int old_s_x = npc.s_x;
	int old_s_y = npc.s_y;

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
	
	if (new_s_x == npc.s_x && new_s_y == npc.s_y) {
	}
	else {
		npc.s_x = new_s_x;
		npc.s_y = new_s_y;

		sector[old_s_x][old_s_y].s_mutex.lock();
		sector[old_s_x][old_s_y].object_list.erase(npc.id);
		sector[old_s_x][old_s_y].s_mutex.unlock();

		sector[new_s_x][new_s_y].s_mutex.lock();
		sector[new_s_x][new_s_y].object_list.insert(npc.id);
		sector[new_s_x][new_s_y].s_mutex.unlock();
	}

	unordered_set<int> new_vl;
	for (auto& o_id : sector[npc.s_x][npc.s_y].object_list) {
		if (PLST_INGAME != objects[o_id].m_state)  continue;
		if (true == is_npc(o_id)) continue;
		
		if (true == can_see(npc.id, o_id)) {
			new_vl.insert(o_id);
		}
	}

	for (auto pl : new_vl) {
		if (0 == old_vl.count(pl)) {
			// 플레이어의 시야에 등장
			objects[pl].m_vl.lock();
			objects[pl].m_view_list.insert(npc.id);
			objects[pl].m_vl.unlock();
			send_add_object(pl, npc.id);
		}
		else {
			// 플레이어가 계속 보고 있음.
			send_move_packet(pl, npc.id);
		}
	}

	for (auto pl : old_vl) {
		if (0 == new_vl.count(pl)) {
			objects[pl].m_vl.lock();
			if (0 != objects[pl].m_view_list.count(npc.id)) {
				objects[pl].m_view_list.erase(npc.id);
				objects[pl].m_vl.unlock();
				objects[npc.id].sleep = true;
				send_remove_object(pl, npc.id);
			}
			else {
				objects[pl].m_vl.unlock();
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
				display_error("GQCS : ", WSAGetLastError());
				exit(-1);
			}
			else {
				display_error("GQCS : ", WSAGetLastError());
				disconnect(key);
			}
		}
		if ((key != SERVER_ID) && (0 == num_bytes)) {
			disconnect(key);
			continue;
		}
		EX_OVER* ex_over = reinterpret_cast<EX_OVER*>(over);

		switch (ex_over->m_op) {
		case OP_RECV: {
			unsigned char* packet_ptr = ex_over->m_packetbuf;
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
				memcpy(ex_over->m_packetbuf, packet_ptr, num_data);
			do_recv(key);
		}
					break;
		case OP_SEND:
			delete ex_over;
			break;
		case OP_ACCEPT:
		{
			int c_id = get_new_player_id(ex_over->m_csocket);
			if (-1 != c_id) {
				objects[c_id].m_recv_over.m_op = OP_RECV;
				objects[c_id].m_prev_size = 0;
				CreateIoCompletionPort(
					reinterpret_cast<HANDLE>(objects[c_id].m_socket), h_iocp, c_id, 0);
				do_recv(c_id);
			}
			else {
				closesocket(objects[c_id].m_socket);
			}

			memset(&ex_over->m_over, 0, sizeof(ex_over->m_over));
			SOCKET c_socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
			ex_over->m_csocket = c_socket;
			AcceptEx(l_socket, c_socket,
				ex_over->m_packetbuf, 0, 32, 32, NULL, &ex_over->m_over);
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
			int move_player = *reinterpret_cast<int*>(ex_over->m_packetbuf);
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
			if (true == is_npc(npc.id)) {
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
			ex_over->m_op = OP_RANDOM_MOVE;
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
		pl.m_state = PLST_FREE;
		pl.sleep = false;
		pl.level = 1;
		pl.max_hp = 100;
		pl.cur_hp = 100;
		pl.max_exp = 100;
		pl.cur_exp = 0;
		pl.power = 10;

		if (true == is_npc(i)) {
			sprintf_s(pl.m_name, "N%d", i);
			pl.m_state = PLST_INGAME;
			pl.x = rand() % WORLD_WIDTH;
			pl.y = rand() % WORLD_HEIGHT;
			pl.s_x = pl.x / SECTOR_X_SIZE;
			pl.s_y = pl.y / SECTOR_Y_SIZE;
			sector[pl.s_x][pl.s_y].object_list.insert(pl.id);

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
	accept_over.m_op = OP_ACCEPT;
	memset(&accept_over.m_over, 0, sizeof(accept_over.m_over));
	SOCKET c_socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	accept_over.m_csocket = c_socket;
	BOOL ret = AcceptEx(listenSocket, c_socket,
		accept_over.m_packetbuf, 0, 32, 32, NULL, &accept_over.m_over);
	if (FALSE == ret) {
		int err_num = WSAGetLastError();
		if (err_num != WSA_IO_PENDING)
			display_error("AcceptEx Error", err_num);
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

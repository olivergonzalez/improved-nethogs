//* NetHogs console UI */
#include <string>
#include <pwd.h>
#include <sys/types.h>
#include <stdlib.h>
#include <cstdlib>
#include <algorithm>

#include <ncurses.h>
#include "nethogs.h"
#include "process.h"


std::string * caption;
//extern char [] version;
const char version[] = " version " VERSION "." SUBVERSION "." MINORVERSION;
extern ProcList * processes;
extern timeval curtime;

extern Process * unknowntcp;
extern Process * unknownudp;
extern Process * unknownip;

// sort on sent or received?
bool sortRecv = true;
// viewMode: kb/s or total
int HEADER_LINE = 0;
int FOOTER_GAP = 1;
int PROCESSES_LINE = 1;
int VIEWMODE_KBPS = 0;
int VIEWMODE_TOTAL_KB = 1;
int VIEWMODE_TOTAL_B = 2;
int VIEWMODE_TOTAL_MB = 3;
int viewMode = VIEWMODE_KBPS;
int nViewModes = 4;
int lastTotalLine = 0;

class Line
{
public:
	Line (const char * name, double n_recv_value, double n_sent_value, pid_t pid, uid_t uid, const char * n_devicename)
	{
		assert (pid >= 0);
		m_name = name;
		sent_value = n_sent_value;
		recv_value = n_recv_value;
		devicename = n_devicename;
		m_pid = pid;
		m_uid = uid;
		assert (m_pid >= 0);
	}

	void show (int row, unsigned int proglen);

	double sent_value;
	double recv_value;
private:
	const char * m_name;
	const char * devicename;
	pid_t m_pid;
	uid_t m_uid;
};

char * uid2username (uid_t uid)
{
	struct passwd * pwd = NULL;
	/* getpwuid() allocates space for this itself,
	 * which we shouldn't free */
	pwd = getpwuid(uid);

	if (pwd == NULL)
	{
		assert(false);
		return strdup ("unlisted");
	} else {
		return strdup(pwd->pw_name);
	}
}


void Line::show (int row, unsigned int proglen)
{
	assert (m_pid >= 0);
	assert (m_pid <= 100000);

	if (DEBUG || tracemode)
	{
		std::cout << m_name << '/' << m_pid << '/' << m_uid << "\t" << sent_value << "\t" << recv_value << std::endl;
		return;
	}

	int processLine = PROCESSES_LINE + row;
	
	if (m_pid == 0)
		mvprintw (processLine, 0, "?");
	else
		mvprintw (processLine, 0, "%d", m_pid);
	char * username = uid2username(m_uid);
	mvprintw (processLine, 6, "%s", username);
	free (username);
	if (strlen (m_name) > proglen) {
		// truncate oversized names
		char * tmp = strdup(m_name);
		char * start = tmp + strlen (m_name) - proglen;
		start[0] = '.';
		start[1] = '.';
		mvprintw (processLine, 6 + 9, "%s", start);
		free (tmp);
	} else {
		mvprintw (processLine, 6 + 9, "%s", m_name);
	}
	mvprintw (processLine, 6 + 9 + proglen + 2, "%s", devicename);
	mvprintw (processLine, 6 + 9 + proglen + 2 + 6, "%10.3f", sent_value);
	mvprintw (processLine, 6 + 9 + proglen + 2 + 6 + 9 + 3, "%10.3f", recv_value);
	if (viewMode == VIEWMODE_KBPS)
	{
		mvprintw (processLine, 6 + 9 + proglen + 2 + 6 + 9 + 3 + 11, "KB/sec");
	}
	else if (viewMode == VIEWMODE_TOTAL_MB)
	{
		mvprintw (processLine, 6 + 9 + proglen + 2 + 6 + 9 + 3 + 11, "MB    ");
	}
	else if (viewMode == VIEWMODE_TOTAL_KB)
	{
		mvprintw (processLine, 6 + 9 + proglen + 2 + 6 + 9 + 3 + 11, "KB    ");
	}
	else if (viewMode == VIEWMODE_TOTAL_B)
	{
		mvprintw (processLine, 6 + 9 + proglen + 2 + 6 + 9 + 3 + 11, "B     ");
	}
}

int GreatestFirst (const void * ma, const void * mb)
{
	Line ** pa = (Line **)ma;
	Line ** pb = (Line **)mb;
	Line * a = *pa;
	Line * b = *pb;
	double aValue;
	if (sortRecv)
	{
		aValue = a->recv_value;
	}
	else
	{
		aValue = a->sent_value;
	}

	double bValue;
	if (sortRecv)
	{
		bValue = b->recv_value;
	}
	else
	{
		bValue = b->sent_value;
	}

	if (aValue > bValue)
	{
		return -1;
	}
	if (aValue == bValue)
	{
		return 0;
	}
	return 1;
}

void init_ui ()
{
	WINDOW * screen = initscr();
	start_color();
	use_default_colors();
	raw();
	noecho();
	cbreak();
	nodelay(screen, TRUE);
	caption = new std::string ("NetHogs");
	caption->append(version);
	//caption->append(", running at ");
	init_pair(1, COLOR_BLACK, COLOR_GREEN);
	init_pair(2, COLOR_BLACK, COLOR_CYAN);
	init_pair(3, COLOR_BLUE, -1);
}

void exit_ui ()
{
	clear();
	endwin();
	delete caption;
}

void ui_tick ()
{
	switch (getch()) {
		case 'q':
			/* quit */
			quit_cb(0);
			break;
		case 's':
			/* sort on 'sent' */
			sortRecv = false;
			break;
		case 'r':
			/* sort on 'received' */
			sortRecv = true;
			break;
		case 'm':
			/* switch mode: total vs kb/s */
			viewMode = (viewMode + 1) % nViewModes;
			break;
	}
}

float tomb (u_int32_t bytes)
{
	return ((double)bytes) / 1024 / 1024;
}
float tokb (u_int32_t bytes)
{
	return ((double)bytes) / 1024;
}
float tokbps (u_int32_t bytes)
{
	return (((double)bytes) / PERIOD) / 1024;
}

/** Get the kb/s values for this process */
void getkbps (Process * curproc, float * recvd, float * sent)
{
	u_int32_t sum_sent = 0,
	  	sum_recv = 0;

	/* walk though all this process's connections, and sum
	 * them up */
	ConnList * curconn = curproc->connections;
	ConnList * previous = NULL;
	while (curconn != NULL)
	{
		if (curconn->getVal()->getLastPacket() <= curtime.tv_sec - CONNTIMEOUT)
		{
			/* stalled connection, remove. */
			ConnList * todelete = curconn;
			Connection * conn_todelete = curconn->getVal();
			curconn = curconn->getNext();
			if (todelete == curproc->connections)
				curproc->connections = curconn;
			if (previous != NULL)
				previous->setNext(curconn);
			delete (todelete);
			delete (conn_todelete);
		}
		else
		{
			u_int32_t sent = 0, recv = 0;
			curconn->getVal()->sumanddel(curtime, &recv, &sent);
			sum_sent += sent;
			sum_recv += recv;
			previous = curconn;
			curconn = curconn->getNext();
		}
	}
	*recvd = tokbps(sum_recv);
	*sent = tokbps(sum_sent);
}

/** get total values for this process */
void gettotal(Process * curproc, u_int32_t * recvd, u_int32_t * sent)
{
	u_int32_t sum_sent = 0,
	  	sum_recv = 0;
	ConnList * curconn = curproc->connections;
	while (curconn != NULL)
	{
		Connection * conn = curconn->getVal();
		sum_sent += conn->sumSent;
		sum_recv += conn->sumRecv;
		curconn = curconn->getNext();
	}
	//std::cout << "Sum sent: " << sum_sent << std::endl;
	//std::cout << "Sum recv: " << sum_recv << std::endl;
	*recvd = sum_recv;
	*sent = sum_sent;
}

void gettotalmb(Process * curproc, float * recvd, float * sent)
{
	u_int32_t sum_sent = 0,
	  	sum_recv = 0;
	gettotal(curproc, &sum_recv, &sum_sent);
	*recvd = tomb(sum_recv);
	*sent = tomb(sum_sent);
}

/** get total values for this process */
void gettotalkb(Process * curproc, float * recvd, float * sent)
{
	u_int32_t sum_sent = 0,
	  	sum_recv = 0;
	gettotal(curproc, &sum_recv, &sum_sent);
	*recvd = tokb(sum_recv);
	*sent = tokb(sum_sent);
}

void gettotalb(Process * curproc, float * recvd, float * sent)
{
	u_int32_t sum_sent = 0,
	  	sum_recv = 0;
	gettotal(curproc, &sum_recv, &sum_sent);
	//std::cout << "Total sent: " << sum_sent << std::endl;
	*sent = sum_sent;
	*recvd = sum_recv;
}

// Display all processes and relevant network traffic using show function
void do_refresh()
{
	int row; // number of terminal rows
	int col; // number of terminal columns
	unsigned int proglen; // max length of the "PROGRAM" column
	
	getmaxyx(stdscr, row, col);	 /* find the boundaries of the screeen */
	if (col < 60) {
		//clear();
		mvprintw(0,0, "The terminal is too narrow! Please make it wider.\nI'll wait...");
		return;
	}

	if (col > PROGNAME_WIDTH) col = PROGNAME_WIDTH;

	proglen = col - 53;

	refreshconninode();
	if (DEBUG || tracemode)
	{
		std::cout << "\nRefreshing:\n";
	}
	else
	{
		// Clear all lines printed last cycle
		for (int i = HEADER_LINE; i <= lastTotalLine; i++) {
			move(i, 0);
			clrtoeol();
		}

		//mvprintw (0, 0, "%s", caption->c_str());
		char header[col];
		sprintf(header, "  PID USER     %-*.*s  DEV        SENT      RECEIVED       ", proglen, proglen, "PROGRAM");
		attron(COLOR_PAIR(1));
		mvprintw (HEADER_LINE, 0, "%s", header);
		attroff(COLOR_PAIR(1));
	}
	ProcList * curproc = processes;
	ProcList * previousproc = NULL;
	int nproc = processes->size();
	/* initialise to null pointers */
	Line * lines [nproc];
	int n = 0, i = 0;
	double sent_global = 0;
	double recv_global = 0;

#ifndef NDEBUG
	// initialise to null pointers
	for (int i = 0; i < nproc; i++)
		lines[i] = NULL;
#endif

	while (curproc != NULL)
	{
		// walk though its connections, summing up their data, and
		// throwing away connections that haven't received a package
		// in the last PROCESSTIMEOUT seconds.
		assert (curproc != NULL);
		assert (curproc->getVal() != NULL);
		assert (nproc == processes->size());

		/* remove timed-out processes (unless it's one of the the unknown process) */
		if ((curproc->getVal()->getLastPacket() + PROCESSTIMEOUT <= curtime.tv_sec)
				&& (curproc->getVal() != unknowntcp)
				&& (curproc->getVal() != unknownudp)
				&& (curproc->getVal() != unknownip))
		{
			if (DEBUG)
				std::cout << "PROC: Deleting process\n";
			ProcList * todelete = curproc;
			Process * p_todelete = curproc->getVal();
			if (previousproc)
			{
				previousproc->next = curproc->next;
				curproc = curproc->next;
			} else {
				processes = curproc->getNext();
				curproc = processes;
			}
			delete todelete;
			delete p_todelete;
			nproc--;
			//continue;
		}
		else
		{
			// add a non-timed-out process to the list of stuff to show
			float value_sent = 0,
				value_recv = 0;

			if (viewMode == VIEWMODE_KBPS)
			{
				//std::cout << "kbps viemode" << std::endl;
				getkbps (curproc->getVal(), &value_recv, &value_sent);
			}
			else if (viewMode == VIEWMODE_TOTAL_KB)
			{
				//std::cout << "total viemode" << std::endl;
				gettotalkb(curproc->getVal(), &value_recv, &value_sent);
			}
			else if (viewMode == VIEWMODE_TOTAL_MB)
			{
				//std::cout << "total viemode" << std::endl;
				gettotalmb(curproc->getVal(), &value_recv, &value_sent);
			}
			else if (viewMode == VIEWMODE_TOTAL_B)
			{
				//std::cout << "total viemode" << std::endl;
				gettotalb(curproc->getVal(), &value_recv, &value_sent);
			}
			else
			{
				forceExit("Invalid viewmode");
			}
			uid_t uid = curproc->getVal()->getUid();
#ifndef NDEBUG
			struct passwd * pwuid = getpwuid(uid);
			assert (pwuid != NULL);
			// value returned by pwuid should not be freed, according to
			// Petr Uzel.
			//free (pwuid);
#endif
			assert (curproc->getVal()->pid >= 0);
			assert (n < nproc);

			lines[n] = new Line (curproc->getVal()->name, value_recv, value_sent,
					curproc->getVal()->pid, uid, curproc->getVal()->devicename);
			previousproc = curproc;
			curproc = curproc->next;
			n++;
#ifndef NDEBUG
			assert (nproc == processes->size());
			if (curproc == NULL)
				assert (n-1 < nproc);
			else
				assert (n < nproc);
#endif
		}
	}

	/* sort the accumulated lines */
	qsort (lines, nproc, sizeof(Line *), GreatestFirst);

	/* print them */
	for (i=0; i<nproc; i++)
	{
		lines[i]->show(i, proglen);
		recv_global += lines[i]->recv_value;
		sent_global += lines[i]->sent_value;
		delete lines[i];
	}
	if (tracemode || DEBUG) {
		/* print the 'unknown' connections, for debugging */
		ConnList * curr_unknownconn = unknowntcp->connections;
		while (curr_unknownconn != NULL) {
			std::cout << "Unknown connection: " <<
				curr_unknownconn->getVal()->refpacket->gethashstring() << std::endl;

			curr_unknownconn = curr_unknownconn->getNext();
		}
	}

	if ((!tracemode) && (!DEBUG)){
		int totalLine = PROCESSES_LINE + i + FOOTER_GAP;

		attron(COLOR_PAIR(2));
		mvprintw (totalLine, 0, "  TOTAL        %-*.*s        %10.3f  %10.3f ", proglen, proglen, " ", sent_global, recv_global);
		if (viewMode == VIEWMODE_KBPS)
		{
			mvprintw (totalLine, col - 7, "KB/sec ");
		} else if (viewMode == VIEWMODE_TOTAL_B) {
			mvprintw (totalLine, col - 7, "B      ");
		} else if (viewMode == VIEWMODE_TOTAL_KB) {
			mvprintw (totalLine, col - 7, "KB     ");
		} else if (viewMode == VIEWMODE_TOTAL_MB) {
			mvprintw (totalLine, col - 7, "MB     ");
		}

		attroff(COLOR_PAIR(2));

		lastTotalLine = totalLine;
		//mvprintw (1+i, 0, "");
		refresh();
	}
}


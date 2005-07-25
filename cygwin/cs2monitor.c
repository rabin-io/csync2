/*
 *  csync2 - cluster synchronisation tool, 2nd generation
 *  LINBIT Information Technologies GmbH <http://www.linbit.com>
 *  Copyright (C) 2004, 2005  Clifford Wolf <clifford@clifford.at>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sqlite.h>
#include <time.h>

static sqlite *db = 0;
static int db_busyc = 0;
static int last_busyc_warn = 0;

static int non_blocking_mode = 0;
static char myhostname[256];
static char *dbname, *dirname;
static time_t restart_time;
static int wheel_counter;

struct service {
	char *name;
	void (*exec_func)();
	time_t timestamp;
	int do_restart;
	int do_panic;
	int pid;
};

static void exec_tcp_listener()
{
	if (non_blocking_mode)
		execl("./csync2.exe", "csync2.exe", "-Biiv", NULL);
	else
		execl("./csync2.exe", "csync2.exe", "-iiv", NULL);
}

static void exec_init_checker()
{
	if (non_blocking_mode)
		execl("./csync2.exe", "csync2.exe", "-Bcrv", "/", NULL);
	else
		execl("./csync2.exe", "csync2.exe", "-crv", "/", NULL);
}

static void exec_checker()
{
	if (non_blocking_mode)
		execl("./csync2.exe", "csync2.exe", "-Bcv", NULL);
	else
		execl("./csync2.exe", "csync2.exe", "-Bcv", NULL);
}

static void exec_updater()
{
	if (non_blocking_mode)
		execl("./csync2.exe", "csync2.exe", "-Buv", NULL);
	else
		execl("./csync2.exe", "csync2.exe", "-uv", NULL);
}

static void exec_hintd()
{
	execl("./cs2hintd.exe", "cs2hintd.exe", dbname, dirname, NULL);
}

static struct service service_tcp_listener = {
	"TCP Listener",
	exec_tcp_listener,
	0, 1, 0, 0
};

static struct service service_init_checker = {
	"Initial Full Checker",
	exec_init_checker,
	0, 0, 0, 0
};

static struct service service_checker = {
	"Incremenmtal Checker",
	exec_checker,
	0, 5, 0, 0
};

static struct service service_updater = {
	"Incremenmtal Updater",
	exec_updater,
	0, 5, 0, 0
};

static struct service service_hintd = {
	"Filesystem Watcher",
	exec_hintd,
	0, 0, 1, 0
};

static struct service *services[] = {
	&service_tcp_listener,
	&service_init_checker,
	&service_checker,
	&service_updater,
	&service_hintd,
	NULL
};

static int got_ctrl_c = 0;

static void ctrl_c_handler(int signum) {
	got_ctrl_c = 1;
}

int main(int argc, char **argv)
{
	signal(SIGINT, ctrl_c_handler);
	signal(SIGTERM, ctrl_c_handler);

	gethostname(myhostname, 256);
	myhostname[255] = 0;

	asprintf(&dbname, "%s.db", myhostname);

	if (argc == 3 && !strcmp(argv[1], "-B")) {
		non_blocking_mode = 1;
		dirname = argv[2];
	} else
	if (argc == 2)
		dirname = argv[1];
	else {
		fprintf(stderr, "Usage: %s [-B] datadir\n", argv[0]);
		return 1;
	}

	{
		int p[2];
		pipe(p);

		if (!fork())
		{
			char *buffer[100], ch;
			int i, pos=0, epos=0;
			time_t last_update = 0;

			dup2(p[0], 0);
			close(p[0]);
			close(p[1]);

			for (i=0; i<100; i++)
				buffer[i] = 0;

			buffer[99] = malloc(256);
			buffer[99][0] = 0;

			while (read(0, &ch, 1) == 1)
			{
				write(1, &ch, 1);

				switch (ch)
				{
				case '\n':
					if (buffer[0])
						free(buffer[0]);
					for (i=1; i<100; i++)
						buffer[i-1] = buffer[i];
					buffer[99] = malloc(256);
					buffer[99][epos=0] = 0;
				case '\r':
					pos = 0;
					break;
				default:
					if (pos < 255) {
						if (++pos > epos)
							epos = pos;
						buffer[99][pos-1] = ch;
						buffer[99][epos] = 0;
					}
				}

				if (last_update+2 < time(0)) {
					FILE *f = fopen("cs2monitor.log", "w");
					if (f) {
						for (i=0; i<100; i++) {
							if (buffer[i])
								fprintf(f, "%s\r\n", buffer[i]);
						}
						fclose(f);
					} else
						fprintf(stderr, "CS2MONITOR: Can't update cs2monitor.log!\n");
					last_update = time(0);
				}
			}

			return 0;
		}

		dup2(p[1], 1);
		dup2(p[1], 2);
		close(p[0]);
		close(p[1]);

		printf("CS2MONITOR: Writing log to cs2monitor.log.\n");
	}

	printf("\n");
	printf("**********************************************************\n");
	printf("\n");
	printf("Csync2 Monitor\n");
	printf("\n");
	printf("Basedir:  %s\n", TRGDIR);
	printf("Datadir:  %s\n", dirname);
	printf("Hostname: %s\n", myhostname);
	printf("Database: %s\n", dbname);
	printf("\n");
	printf("**********************************************************\n");
	printf("\n");
	fflush(stdout);

	if (chdir(TRGDIR) < 0)
		goto io_error;

	printf("CS2MONITOR: Killing all running 'csync2' processes...\n");
	system("./killall.exe csync2 2> /dev/null");

	printf("CS2MONITOR: Killing all running 'cs2hintd' processes...\n");
	system("./killall.exe cs2hintd 2> /dev/null");

	printf("CS2MONITOR: Killing all running 'cs2hintd_fseh' processes...\n");
	system("./killall.exe cs2hintd_fseh 2> /dev/null");

	if (0) {
		struct service **s;
restart_entry_point:
		for (s = services; *s; s++)
			(*s)->pid = 0;
	}

	fflush(stdout);
	sleep(1);

	{
		char vacuum_command[strlen(dbname) + 100];
		sprintf(vacuum_command, "./sqlite.exe %s vacuum", dbname);

		printf("CS2MONITOR: Running database VACUUM command...\n");
		system(vacuum_command);
	}

	{
		unsigned char random_number = 0;
		int rand = open("/dev/random", O_RDONLY);
		read(rand, &random_number, sizeof(unsigned char));
		close(rand);

		restart_time = 60 + random_number%30;
		printf("CS2MONITOR: Automatic restart in %d minutes.\n", (int)restart_time);
		restart_time = time(0) + restart_time * 60;
        }

	while (1)
	{
		struct timespec interval;
		time_t remaining_restart_time;
		struct service **s;
		int rc;

		for (s = services; *s; s++)
		{
			/* never has been started before */
			if ((*s)->pid == 0)
			{
				printf("CS2MONITOR: [%ld] Running job '%s' ...\n", (long)time(0), (*s)->name);
				fflush(stdout);
				if (((*s)->pid = fork()) == 0) {
					(*s)->exec_func();
					goto io_error;
				}
			}
			else
			/* has been terminated */
			if ((*s)->pid == -1)
			{
				if ((*s)->do_restart && time(0) > ((*s)->timestamp + (*s)->do_restart)) {
					printf("CS2MONITOR: [%ld] Running job '%s' ...\n", (long)time(0), (*s)->name);
					fflush(stdout);
					if (((*s)->pid = fork()) == 0) {
						(*s)->exec_func();
						goto io_error;
					}
				}
			}
			else
			/* is running or a zombie */
			{
				int waitpid_status, waitpid_rc;
				waitpid_rc = waitpid((*s)->pid, &waitpid_status, WNOHANG);

				if (waitpid_rc != 0) {
					(*s)->pid = -1;
					(*s)->timestamp = time(0);

					if ((*s)->do_panic) {
						printf("CS2MONITOR: Job '%s' terminated! Restarting CS2MONITOR..\n", (*s)->name);
						goto panic_restart_everything;
					}
				}
			}
		}

		interval.tv_sec = 0;
		interval.tv_nsec = 250000000;
		nanosleep(&interval, 0);

		wheel_counter = (wheel_counter+1) % 4;
		remaining_restart_time = restart_time - time(0);

		printf("[%02d:%02d] %c\r", 
			(int)(remaining_restart_time / 60),
			(int)(remaining_restart_time % 60),
			"/-\\|"[wheel_counter]);
		fflush(stdout);

		if (remaining_restart_time <= 0) {
			printf("CS2MONITOR: Restarting CS2MONITOR now...\n");
			goto panic_restart_everything;
		}

		if (got_ctrl_c) {
			printf("CS2MONITOR: Got Ctrl-C signal. Terminating...\n");
			goto panic_restart_everything;
		}

		db = sqlite_open(dbname, 0, 0);
		if (!db) {
			printf("CS2MONITOR: Can't open database file! Restarting CS2MONITOR..\n");
			goto panic_restart_everything;
		}

		rc = sqlite_exec(db, "BEGIN TRANSACTION", 0, 0, 0);

		if ( rc != SQLITE_BUSY && rc != SQLITE_OK ) {
			printf("CS2MONITOR: Got database error %d on DB check! Restarting CS2MONITOR..\n", rc);
			sqlite_close(db);
			goto panic_restart_everything;
		}
			
		if ( rc == SQLITE_BUSY ) {
			db_busyc++;
			if (db_busyc > 600) {
				printf("CS2MONITOR: Database is busy for 600 seconds! Restarting CS2MONITOR..\n");
				sqlite_close(db);
				goto panic_restart_everything;
			}
			if (db_busyc > 300 || db_busyc > last_busyc_warn + 10) {
				printf("CS2MONITOR: DB is busy for %d seconds now (Monitor restart at 600 seconds).\n", db_busyc);
				sqlite_close(db);
				last_busyc_warn = db_busyc;
			}
		} else {
			sqlite_exec(db, "COMMIT TRANSACTION", 0, 0, 0);
			db_busyc = last_busyc_warn = 0;
		}

		sqlite_close(db);
	}

panic_restart_everything:
	fflush(stdout);
	sleep(1);

	printf("CS2MONITOR: Killing all running 'csync2' processes...\n");
	system("./killall.exe csync2 2> /dev/null");

	printf("CS2MONITOR: Killing all running 'cs2hintd' processes...\n");
	system("./killall.exe cs2hintd 2> /dev/null");

	printf("CS2MONITOR: Killing all running 'cs2hintd_fseh' processes...\n");
	system("./killall.exe cs2hintd_fseh 2> /dev/null");

	fflush(stdout);
	sleep(5);

	while (waitpid(-1, 0, WNOHANG) > 0) {};

	if (got_ctrl_c) {
		printf("CS2MONITOR: Bye.\n");
		fflush(stdout);
		sleep(1);
		return 0;
	}

	printf("CS2MONITOR: Restarting...\n");

	fflush(stdout);
	sleep(1);

	goto restart_entry_point;

io_error:
	fprintf(stderr, "CS2MONITOR I/O Error: %s\n", strerror(errno));
	return 1;
}


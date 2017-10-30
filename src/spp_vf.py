#!/usr/bin/python
"""Soft Patch Panel"""

from __future__ import print_function
from Queue import Queue
from thread import start_new_thread
from threading import Thread
import cmd
import getopt
import select
import socket
import sys

import json

class GrowingList(list):
    """GrowingList"""

    def __setitem__(self, index, value):
        if index >= len(self):
            self.extend([None]*(index + 1 - len(self)))
        list.__setitem__(self, index, value)

MAX_SECONDARY = 16

# init
PRIMARY = ''
SECONDARY_LIST = []
SECONDARY_COUNT = 0

#init primary comm channel
MAIN2PRIMARY = Queue()
PRIMARY2MAIN = Queue()

#init secondary comm channel list
MAIN2SEC = GrowingList()
SEC2MAIN = GrowingList()

def connectionthread(name, client_id, conn, m2s, s2m):
    """Manage secondary process connections"""

    cmd_str = 'hello'

    #infinite loop so that function do not terminate and thread do not end.
    while True:
        try:
            _, _, _ = select.select([conn,], [conn,], [], 5)
        except select.error:
            break

        #Sending message to connected secondary
        try:
            cmd_str = m2s.get(True)
            conn.send(cmd_str) #send only takes string
        except KeyError:
            break
        except Exception, excep:
            print (str(excep))
            break

        #Receiving from secondary
        try:
            data = conn.recv(1024) # 1024 stands for bytes of data to be received
            if data:
                s2m.put("recv:" + str(conn.fileno()) + ":" + "{" + data + "}")
            else:
                s2m.put("closing:" + str(conn))
                break
        except Exception, excep:
            print (str(excep))
            break

    SECONDARY_LIST.remove(client_id)
    conn.close()

def getclientid(conn):
    """Get client_id from client"""

    try:
        conn.send("_get_client_id")
        #conn.send("{\"commands\":[{\"command\":\"process\"}]}")
    except KeyError:
        return -1

    data = conn.recv(1024)
    if data == None:
        return -1

    #client_id = int(data.strip('\0'))
    json_dict = json.loads(data)
    client_id = int(json_dict['client_id'])

    if client_id < 0 or client_id > MAX_SECONDARY:
        return -1

    print ("secondary id %d" % client_id)
    return client_id

    found = 0
    for i in SECONDARY_LIST:
        if client_id == i:
            found = 1
            break

    if found == 0:
        return client_id

    # client_id in use, find a free one
    free_client_id = -1
    for i in range(MAX_SECONDARY):
        found = -1
        for j in SECONDARY_LIST:
            if i == j:
                found = i
                break
        if found == -1:
            free_client_id = i
            break

    if free_client_id < 0:
        return -1

    conn.send("_set_client_id %u" % free_client_id)
    data = conn.recv(1024)

    return free_client_id

def acceptthread(sock, main2sec, sec2main):
    """Listen for secondary processes"""

    global SECONDARY_COUNT

    try:
        while True:
            #Accepting incoming connections
            conn, _ = sock.accept()

            client_id = getclientid(conn)
            if client_id < 0:
                break

            #Creating new thread.
            #Calling secondarythread function for this function and passing
            #conn as argument.

            SECONDARY_LIST.append(client_id)
            main2sec[client_id] = Queue()
            sec2main[client_id] = Queue()
            start_new_thread(connectionthread,
                             ('secondary', client_id, conn,
                              main2sec[client_id],
                              sec2main[client_id], ))
            SECONDARY_COUNT += 1
    except Exception, excep:
        print (str(excep))
        sock.close()

def command_primary(command):
    """Send command to primary process"""

    if PRIMARY:
        MAIN2PRIMARY.put(command)
        print (PRIMARY2MAIN.get(True))
    else:
        print ("primary not started")

def command_secondary(sec_id, command):
    """Send command to secondary process with sec_id"""

    if sec_id in SECONDARY_LIST:
        MAIN2SEC[sec_id].put(command)
        print (SEC2MAIN[sec_id].get(True))
    else:
        print ("secondary id %d not exist" % sec_id)

def print_status():
    """Display information about connected clients"""

    print ("Soft Patch Panel Status :")
    print ("primary: %d" % PRIMARY)
    print ("secondary count: %d" % len(SECONDARY_LIST))
    for i in SECONDARY_LIST:
        print ("Connected secondary id: %d" % i)

def primarythread(sock, main2primary, primary2main):
    """Manage primary process connection"""

    global PRIMARY
    cmd_str = ''

    while True:
        #waiting for connection
        PRIMARY = False
        conn, addr = sock.accept()
        PRIMARY = True

        while conn:
            try:
                _, _, _ = select.select([conn,], [conn,], [], 5)
            except select.error:
                break

            #Sending message to connected primary
            try:
                cmd_str = main2primary.get(True)
                conn.send(cmd_str) #send only takes string
            except KeyError:
                break
            except Exception, excep:
                print (str(excep))
                break

            #Receiving from primary
            try:
                data = conn.recv(1024) # 1024 stands for bytes of data to be received
                if data:
                    primary2main.put("recv:" + str(addr) + ":" + "{" + data + "}")
                else:
                    primary2main.put("closing:" + str(addr))
                    conn.close()
                    break
            except Exception, excep:
                print (str(excep))
                break

    print ("primary communication thread end")

def close_all_secondary():
    """Exit all secondary processes"""

    return;

    global SECONDARY_COUNT

    tmp_list = []
    for i in SECONDARY_LIST:
        tmp_list.append(i)
    for i in tmp_list:
        command_secondary(i, 'exit')
    SECONDARY_COUNT = 0

def check_sec_cmds(cmds):
    """Validate secondary commands before sending"""

    return 1

    level1 = ['status', 'exit', 'forward', 'stop']
    level2 = ['add', 'patch', 'del']
    patch_args = ['reset']
    add_del_args = ['ring', 'vhost']
    cmdlist = cmds.split(' ')
    valid = 0

    length = len(cmdlist)
    if length == 1:
        if cmdlist[0] in level1:
            valid = 1
    elif length == 2:
        if cmdlist[0] == 'patch':
            if cmdlist[1] in patch_args:
                valid = 1
    elif length == 3:
        if cmdlist[0] in level2:
            if cmdlist[0] == 'add' or cmdlist[0] == 'del':
                if cmdlist[1] in add_del_args:
                    if str.isdigit(cmdlist[2]):
                        valid = 1
            elif cmdlist[0] == 'patch':
                if str.isdigit(cmdlist[1]) and str.isdigit(cmdlist[2]):
                    valid = 1

    return valid

class Shell(cmd.Cmd):
    """SPP command prompt"""

    intro = 'Welcome to the spp.   Type help or ? to list commands.\n'
    prompt = 'spp > '
    recorded_file = None

    COMMANDS = ['status', 'add', 'patch', 'ring', 'vhost',
                'reset', 'exit', 'forward', 'stop', 'clear']

    def complete_pri(self, text, line, begidx, endidx):
        """Completion for primary process commands"""

        if not text:
            completions = self.COMMANDS[:]
        else:
            completions = [p
                           for p in self.COMMANDS
                           if p.startswith(text)
                          ]
        return completions

    def complete_sec(self, text, line, begidx, endidx):
        """Completion for secondary process commands"""

        if not text:
            completions = self.COMMANDS[:]
        else:
            completions = [p
                           for p in self.COMMANDS
                           if p.startswith(text)
                          ]
        return completions

    def do_status(self, _):
        """Display Soft Patch Panel Status"""

        print_status()

    def do_pri(self, command):
        """Send command to primary process"""

        if command and command in self.COMMANDS:
            command_primary(command)
        else:
            print ("primary invalid command")

    def do_sec(self, arg):
        """Send command to secondary process"""

        cmds = arg.split(';')
        if len(cmds) < 2:
            print ("error")
        elif str.isdigit(cmds[0]):
            sec_id = int(cmds[0])
            if check_sec_cmds(cmds[1]):
                command_secondary(sec_id, cmds[1])
            else:
                print ("invalid cmd")
        else:
            print (cmds[0])
            print ("first %s" % cmds[1])

    def do_record(self, arg):
        """Save future commands to filename:  RECORD filename.cmd"""

        self.recorded_file = open(arg, 'w')

    def do_playback(self, arg):
        """Playback commands from a file:  PLAYBACK filename.cmd"""

        self.close()
        try:
            with open(arg) as recorded_file:
                lines = []
                for line in recorded_file:
                    if line.strip().startswith("#"):
                        continue
                    lines.append(line)
                self.cmdqueue.extend(lines)
        except IOError:
            print ("Error: File does not exist.")

    def precmd(self, line):
        line = line.lower()
        if self.recorded_file and 'playback' not in line:
            print(line, file=self.recorded_file)
        return line

    def close(self):
        """Close record file"""

        if self.recorded_file:
            print("closing file")
            self.recorded_file.close()
            self.recorded_file = None

    def do_bye(self, arg):
        """Stop recording, close SPP, and exit: BYE"""

        cmds = arg.split(' ')
        if cmds[0] == 'sec':
            close_all_secondary()
        elif cmds[0] == 'all':
            close_all_secondary()
            command_primary('exit')
        elif cmds[0] == '':
            print('Thank you for using Soft Patch Panel')
            self.close()
            return True

def main(argv):
    """main"""

    # Defining server address and port
    host = ''  #'localhost' or '127.0.0.1' or '' are all same

    try:
        opts, _ = getopt.getopt(argv, "p:s:h", ["help", "primary = ", "secondary"])
    except getopt.GetoptError:
        print ('spp.py -p <primary__port_number> -s <secondary_port_number>')
        sys.exit(2)
    for opt, arg in opts:
        if opt in ("-h", "--help"):
            print ('spp.py -p <primary__port_number> -s <secondary_port_number>')
            sys.exit()
        elif opt in ("-p", "--primary"):
            primary_port = int(arg)
            print ("primary port : %d" % primary_port)
        elif opt in ("-s", "--secondary"):
            secondary_port = int(arg)
            print ('secondary port : %d' % secondary_port)

    #Creating primary socket object
    primary_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)

    #Binding primary socket to a address. bind() takes tuple of host and port.
    primary_sock.bind((host, primary_port))

    #Listening primary at the address
    primary_sock.listen(1) #5 denotes the number of clients can queue

    primary_thread = Thread(target=primarythread,
                            args=(primary_sock, MAIN2PRIMARY, PRIMARY2MAIN,))
    primary_thread.daemon = True
    primary_thread.start()

    #Creating secondary socket object
    secondary_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)

    #Binding secondary socket to a address. bind() takes tuple of host and port.
    secondary_sock.bind((host, secondary_port))

    #Listening secondary at the address
    secondary_sock.listen(MAX_SECONDARY)

    # secondary process handling thread
    start_new_thread(acceptthread, (secondary_sock, MAIN2SEC, SEC2MAIN))

    shell = Shell()
    shell.cmdloop()
    shell = None

    primary_sock.shutdown(socket.SHUT_RDWR)
    primary_sock.close()
    secondary_sock.shutdown(socket.SHUT_RDWR)
    secondary_sock.close()

if __name__ == "__main__":
    main(sys.argv[1:])

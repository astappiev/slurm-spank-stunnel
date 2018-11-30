/***************************************************************************\
 slurm-spank-stunnel.c - SLURM SPANK TUNNEL plugin
 ***************************************************************************
 * Copyright  Harvard University (2014)
 * Copyright  Quantum HPC Inc. (2018)
 *
 * Written by Aaron Kitzmiller <aaron_kitzmiller@harvard.edu> based on
 * X11 SPANK by Matthieu Hautreux <matthieu.hautreux@cea.fr>
 * Modified by Quentin Lux <qlux@quantumhpc.com>
 *
 * This file is part of slurm-spank-stunnel, a SLURM SPANK Plugin aiming at
 * providing arbitrary port forwarding on SLURM execution
 * nodes using OpenSSH.
 *
 * slurm-spank-stunnel is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * slurm-spank-stunnel is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with slurm-spank-stunnel; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
\***************************************************************************/
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pwd.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <stdint.h>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>

#include <slurm/slurm.h>
#include <slurm/spank.h>


#define STUNNEL_ENVVAR		 			"SLURM_STUNNEL"
/*
*  Determine whether ssh tunnels are started from a local or remote context
*
*  = 1   remote context, i.e. tunnel started from first compute node
*  = 0   not remote context, tunnel started from srun machine submission
*/
#define STUNNEL_MODE		 				"STUNNEL_MODE"
#define STUNNEL_CONTROL_FILE		"STUNNEL_CONTROL_FILE"

#define INFO slurm_info
#define DEBUG slurm_debug
#define ERROR slurm_error


static char* ssh_cmd = NULL;
static char* ssh_args = NULL;
static char* helpertask_args = NULL ;
static char* args = NULL;
static char* tunnel_host = NULL;
static char* tunnel_controlfile = NULL;

/*
 * can be used to adapt the ssh parameters to use to
 * set up the ssh tunnel
 *
 * this can be overriden by ssh_cmd= and args=
 * spank plugin conf args
 */
#define DEFAULT_SSH_CMD "ssh"
#define DEFAULT_SSH_ARGS ""
#define DEFAULT_HELPERTASK_ARGS ""
#define DEFAULT_ARGS ""

/*
 * string pattern for file that stores the remote hostname needed for the ssh
 * control commands
 */
#define HOST_FILE_PATTERN	   "/tmp/%s-host.tunnel"

/*
 * string pattern for file used as the ssh control master file
 */
#define CONTROL_FILE_PATTERN	"/tmp/%d-%d-control.tunnel"

/*
 * All spank plugins must define this macro for the SLURM plugin loader.
 */
SPANK_PLUGIN(stunnel, 1);

/*
 * Returns 1 if port is free, 0 otherwise
 *
 */
int port_available(int port)
{
	DEBUG("STUNNEL=> TESTING PORT %d", port);
	int result = 1;

	struct sockaddr_in serv_addr;
	int sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if( sockfd < 0 ) {
		ERROR("STUNNEL=> Error getting socket for port check.");
		return 0;
	}

	bzero((char *) &serv_addr, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = INADDR_ANY;
	serv_addr.sin_port = htons(port);
	int bindresult = bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr));
	if (bindresult  < 0) {
		result = 0;
	}

	if (close (sockfd) < 0 ) {
		ERROR("STUNNEL=> Close of socket during port test failed?? fd: %s", strerror(errno));
		result = 0;
	}
	return result;
}

int file_exists(char *filename){
	struct stat buf;
	return (stat(filename,&buf) == 0);
}

/*
 * Writes the file that records the hostname
 */
// int write_host_file(spank_t spank, char *host)
// {
//
// 	DEBUG("writing host file");
// 	FILE* file;
// 	char filename[256];
// 	char *user = getenv("USER");
//
// 	// build file reference
// 	if ( snprintf(filename,256,HOST_FILE_PATTERN,user) >= 256 ) {
// 		ERROR("Error: Unable to build file reference");
// 		return 20;
// 	}
//
// 	// this file shouldn't exist, so warn
// 	if (file_exists(filename)){
// 		ERROR("Warning: The hostname file %s exists and will be overwritten.  There may stray ssh processes that should be killed.", filename);
// 	}
//
// 	// write it into reference file
// 	file = fopen(filename,"w");
// 	if ( file == NULL ) {
// 		ERROR("error: unable to create file %s", filename);
// 		return 30;
// 	}
//
// 	fprintf(file,"%s",host);
// 	fclose(file);
// 	return 0;
// }
//
// /*
//  * Reads the host file so that the ssh tunnel can be terminated.  Deletes the
//  * host file when it's done reading.
//  */
// int read_host_file(char *buf)
// {
//
// 	DEBUG("parsing host file");
// 	FILE* file;
// 	char filename[256];
// 	char *user = getenv("USER");
//
// 	// build file reference
// 	if ( snprintf(filename,256,HOST_FILE_PATTERN,user) >= 256 ) {
// 		ERROR("tunnel: unable to build file reference");
// 		return 20;
// 	}
// 	file = fopen(filename,"r");
// 	if ( file == NULL ) {
// 		// ERROR("tunnel: unable to read file %s. You may need to manually kill ssh tunnel processes.", filename);
// 		return 30;
// 	}
//
// 	//Read the lines of the host file
// 	char line[100];
// 	if (fgets(line,100,file) == NULL) {
// 		ERROR("Unable to read from file %s",filename);
// 	}
// 	if (line[strlen(line) - 1] == '\n') {
// 		line[strlen(line) - 1] = '\0';
// 	}
// 	snprintf(buf,100,"%s",line);
// 	fclose(file);
// 	unlink(filename);
// 	return 0;
// }

/*
 * Build an SSH control file based on the pattern with UID and JOBID
 */
int _build_control_file (spank_t spank)
{
	uid_t uid;
	uint32_t jobid;
	size_t controlfile_length;
	if(spank_get_item (spank, S_JOB_UID, &uid) != ESPANK_SUCCESS ) {
		DEBUG("STUNNEL=> Cannot get user id");
		exit(1);
	}

	if(spank_get_item (spank, S_JOB_ID, &jobid) != ESPANK_SUCCESS ) {
		DEBUG("STUNNEL=> Cannot get job id");
		exit(1);
	}

	controlfile_length = strlen(CONTROL_FILE_PATTERN) + 128;
	tunnel_controlfile = (char*) malloc(controlfile_length*sizeof(char));

	// Setup the control file name
	snprintf(tunnel_controlfile,controlfile_length,CONTROL_FILE_PATTERN, uid, jobid);

	DEBUG("STUNNEL=> ===>control file %s",tunnel_controlfile);
	return tunnel_controlfile == NULL;
}

/*
 * This does the actual port forward.  An ssh control master file is used
 * when the connection is established so that it can be terminated later.
 */
int _connect_node (spank_t spank, char* node)
{
	DEBUG("STUNNEL=> CONNECTING NODE : %s",node);

	int status = -1;
	char* expc_pattern="%s %s %s %s -f -N -M -S %s %s";
	char* expc_cmd;
	size_t expc_length;

	// If this control file already exists on this submit host, bail out
	if (file_exists(tunnel_controlfile)) {
		ERROR("STUNNEL=> ssh control file %s already exists. Either you already have a tunnel in place, or one did not terminate correctly. Please remove this file.", tunnel_controlfile);
		exit(1);
	}

	// sshcmd is already set
	expc_length = strlen(node) +
								strlen(ssh_cmd) +
								strlen(ssh_args) +
								strlen(args) +
								strlen(tunnel_controlfile) +
								strlen(helpertask_args) +
								20;
	expc_cmd = (char*) malloc(expc_length*sizeof(char));
	if ( expc_cmd != NULL ) {
		snprintf(expc_cmd,expc_length,expc_pattern,
				(ssh_cmd == NULL) ? DEFAULT_SSH_CMD : ssh_cmd,
				(ssh_args == NULL) ? DEFAULT_SSH_ARGS : ssh_args,
				node,
				args,
				tunnel_controlfile,
				(helpertask_args == NULL) ? DEFAULT_HELPERTASK_ARGS : helpertask_args);
		DEBUG("STUNNEL=> ssh_cmd : %s", expc_cmd);
		status = system(expc_cmd);
		if ( status == -1 ){
			  ERROR("STUNNEL=> tunnel: unable to connect node %s with command %s",node,expc_cmd);
		}else{
			  // Write the hostname to a file
			  // write_host_file(spank, node);
		}
		free(expc_cmd);
	}
	if (args != NULL){
		free(args);
	}

	return status;
}

/*
 * Takes the first of the allocated nodes and passes to _connect_node
 *
 */
int _stunnel_connect_nodes (spank_t spank, char* nodes)
{
	// char* host;
	hostlist_t hlist;

	// Connect to the first host in the list
	hlist = slurm_hostlist_create(nodes);
	tunnel_host = slurm_hostlist_shift(hlist);
	_connect_node(spank, tunnel_host);
	slurm_hostlist_destroy(hlist);

	return 0;
}

/*
 * Generate ssh tunnels from a REMOTE context
 * ComputeNode --> Submit Host in Remote Forwarding (default)
 *
 */

int slurm_spank_task_init(spank_t spank, int ac, char **av)
// int slurm_spank_user_init (spank_t spank, int ac, char **av) //root ?
{
	if (!spank_remote (spank)){
		return 0;
	}

	// If there are no ssh args, then there is nothing to do
	if (args == NULL){
		goto exit;
	}

	char tunnel_mode[1024];
	if (spank_getenv(spank, STUNNEL_MODE, tunnel_mode, sizeof(tunnel_mode)) == ESPANK_SUCCESS){
			DEBUG("STUNNEL=> MODE======= %s", tunnel_mode);
			// Tunnels are handled locally, skip
			if(strcmp(tunnel_mode,"0") == 0){
				DEBUG("STUNNEL=> Tunnels are started locally from submission host, skip");
				return 0;
			}
	}else{
		// No option provided
		return 0;
	}

	int status = 0;

	// Remote job is connecting to the submit host
	tunnel_host = (char*) malloc(1024*sizeof(char));

	if (spank_getenv(spank, "SLURM_SUBMIT_HOST", tunnel_host, 1024) == ESPANK_SUCCESS){
		DEBUG("STUNNEL=> Submit host === %s",tunnel_host);
	}else{
		ERROR("STUNNEL=> Cannot retrieve submission host name. Exiting...");
		exit(1);
	}

	// connect submit host
	status = 	_connect_node(spank, tunnel_host);

	clean_exit:

	exit:
	return status;
}

/*
 * Generate ssh tunnels from a LOCAL context (srun)
 * Submit Host --> First Compute Node in Local Forwarding (default)
 * (_stunnel_connect_nodes, _connect_node)
 * This function is not used when using sbatch/salloc
 *
 */
int slurm_spank_local_user_init (spank_t spank, int ac, char **av)
{
	// nothing to do in remote mode
	if (spank_remote (spank)){
		return 0;
	}

	// If there are no ssh args, then there is nothing to do
	if (args == NULL){
		goto exit;
	}

	// Local mode
	if (setenv(STUNNEL_MODE, "0", 1) < 0) {
      ERROR("STUNNEL=> Cannot set stunnel mode");
      return -1;
  }
	// For LOCAL srun, build control file here
	if(_build_control_file(spank)){
		ERROR("STUNNEL=> Cannot build controlfile name");
		exit(1);
	}

	int status = 0;

	uint32_t jobid;
	job_info_msg_t * job_buffer_ptr;
	job_info_t* job_ptr;

	// get job id
	if ( spank_get_item (spank, S_JOB_ID, &jobid)
		 != ESPANK_SUCCESS ) {
		status = -1;
		goto exit;
	}

	// get job infos
	status = slurm_load_job(&job_buffer_ptr,jobid,SHOW_ALL);
	if ( status != 0 ) {
		ERROR("STUNNEL=> stunnel: unable to get job infos");
		status = -3;
		goto exit;
	}

	// check infos validity
	if ( job_buffer_ptr->record_count != 1 ) {
		ERROR("STUNNEL=> stunnel: job infos are invalid");
		status = -4;
		goto clean_exit;
	}
	job_ptr = job_buffer_ptr->job_array;

	// check allocated nodes var
	if ( job_ptr->nodes == NULL ) {
		ERROR("STUNNEL=> stunnel: job has no allocated nodes defined");
		status = -5;
		goto clean_exit;
	}

	// connect required nodes
	status = _stunnel_connect_nodes(spank, job_ptr->nodes);

	clean_exit:
	slurm_free_job_info_msg(job_buffer_ptr);

	exit:
	return status;
}

/*
 * The termination command is:
 *
 *	   ssh <hostname> -S <tunnel_controlfile> -O exit >/dev/null 2>&1
 *
 * The controlfile is, as established above, based on the uid and jobid.
 *
 * srun : the command is excuted by the user locally
 * sbatch/salloc: the command is executed by root/the user running slurmd
 * 				which should habe permission to terminate the tunnel for the user
 *
 */

int close_tunnel(spank_t spank)
{
	// Control file will be set only on the machine having started the tunnel
	if(tunnel_controlfile == NULL){
		DEBUG("STUNNEL=> Control file is empty, nothing to do");
		return 0;
	}

	DEBUG("STUNNEL=> Control file is %s", tunnel_controlfile);
	DEBUG("STUNNEL=> Tunnel host is %s", tunnel_host);

	// If the control file isn't there, don't do anything
	if (!file_exists(tunnel_controlfile)){
		ERROR("STUNNEL=> Control file %s does not exist",tunnel_controlfile);
		return 0;
	}


	char* expc_cmd;
	char* expc_pattern = "%s %s %s -S %s -O exit >/dev/null 2>&1";
	size_t expc_length;

	int status = -1;

	// Read the host file so the ssh command has a host
	// char host[1000];
	// read_host_file(host);
	// if (strcmp(host, "") == 0){
	// 	//ERROR("STUNNEL=> empty host file");
	// 	return 0;
	// }

	// remove background ssh tunnels
	expc_length = strlen(expc_pattern) + 128 ;
	expc_cmd = (char*) malloc(expc_length*sizeof(char));
	if ( expc_cmd != NULL &&
			( snprintf(expc_cmd,expc_length,expc_pattern,
				(ssh_cmd == NULL) ? DEFAULT_SSH_CMD : ssh_cmd,
				(ssh_args == NULL) ? DEFAULT_SSH_ARGS : ssh_args,
				tunnel_host,
				tunnel_controlfile)
					>= expc_length )	) {
		ERROR("STUNNEL=> tunnel: error while creating kill cmd");
	}
	else {

		DEBUG("STUNNEL=> ssh_cmd : %s", expc_cmd);
		status = system(expc_cmd);
		if ( status == -1 ) {
			ERROR("STUNNEL=> tunnel: unable to exec kill cmd %s",expc_cmd);
		}
	}

	if ( expc_cmd != NULL ){
		free(expc_cmd);
	}

	return status;
}
 /*
  * Termination for REMOTE context with sbatch/salloc through task end
	*
	*/
int slurm_spank_task_exit(spank_t spank, int ac, char **av)
{
	if (!spank_remote (spank)){
		return 0;
	}

	char tunnel_mode[1024];
	if (spank_getenv(spank, STUNNEL_MODE, tunnel_mode, sizeof(tunnel_mode)) == ESPANK_SUCCESS){
			DEBUG("STUNNEL=> MODE======= %s", tunnel_mode);
			// Tunnels are handled locally, skip
			if(strcmp(tunnel_mode,"0") == 0){
				DEBUG("STUNNEL=> Tunnels are started locally from submission host, skip");
				return 0;
			}
	}else{
		// No option provided
		return 0;
	}
	tunnel_host = (char*) malloc(1024*sizeof(char));

	if (spank_getenv(spank, "SLURM_SUBMIT_HOST", tunnel_host, 1024) == ESPANK_SUCCESS){
		DEBUG("STUNNEL=> Submit host === %s",tunnel_host);
	}else{
		ERROR("STUNNEL=> Cannot retrieve submission host name. Exiting...");
		// exit(1);
	}

	return close_tunnel(spank);
}

 /*
  * Termination for LOCAL context with srun
	*
	*/
int slurm_spank_exit (spank_t spank, int ac, char **av)
{
	if (spank_remote (spank)){
		return 0;
	}
	return close_tunnel(spank);
}


/*
 * Uses the contents of the --tunnel option to create args string consisting of
 * -L <submit host>:localhost:<exec host>.  There may be multiple -L options.
 */
static int _tunnel_opt_process (int val, const char *optarg, int remote)
{
	DEBUG("STUNNEL=> ================: OPT called %d",remote);

	if (optarg == NULL) {
		ERROR("STUNNEL=> --tunnel requires an argument, e.g. 8888:8888");
		return (0);
	}
	args = (char *)calloc(1024, sizeof(char));
	char *portlist = strdup(optarg);
	int portpaircount = 1;
	int i = 0;
	for (i=0; i < strlen(portlist); i++){
		if (portlist[i] == ','){
			portpaircount++;
		}
	}

	//Break up the string by comma to get the list of port pairs
	char **portpairs = malloc(portpaircount * sizeof(char*));
	char *ptr;

	char *token  = strtok_r(portlist,",",&ptr);
	int numpairs = 0;
	while (token != NULL){
		portpairs[numpairs] = strdup(token);
		token = strtok_r(NULL,",",&ptr);
		numpairs++;
	}
	free(portlist);

	//Go through the port pairs and create the switch string
	if (numpairs == 0){
		return (0);
	}
	DEBUG("STUNNEL=> %d tunnel pairs specified", numpairs);

	for (i=0; i<numpairs; i++){
		// char *directioon = strtok_r(portpairs[i],":",&ptr);
		char *firststr = strtok_r(portpairs[i],":",&ptr);
		char *hoststr = strtok_r(NULL,":",&ptr);

		if (hoststr == NULL){
			ERROR("STUNNEL=> --tunnel parameter needs two numeric ports separated by a colon");
			free(portpairs);
			exit(1);
		}

		// Optionnal destination host - default to localhost
		char *secondstr = strtok_r(NULL,":",&ptr);
		if (secondstr == NULL){
			secondstr=hoststr;
			hoststr="localhost";
		}

		int first;
		int second;
		char *p;
		char *to = malloc(sizeof(char));

		DEBUG("STUNNEL=> FIRST PORT : %s", firststr);
		DEBUG("STUNNEL=> SECOND PORT : %s", secondstr);
		DEBUG("STUNNEL=> HOST : %s", hoststr);
		strncpy(to, firststr, 1);
		to[1]='\0';

		if (strcmp(to,"R") == 0 || strcmp(to,"L") == 0){
			firststr+=1;
		}else{
			if (spank_context() == S_CTX_REMOTE){
				// Default is remote port forwarding
				to="R";
			}else if(spank_context() == S_CTX_LOCAL){
				// Default is Local port forwarding
				to="L";
			}
		}
		first = atoi(firststr);
		second = atoi(secondstr);


		if (first == 0 || second == 0){
			ERROR("STUNNEL=> --tunnel parameter requires two numeric ports separated by a colon");
			free(portpairs);
			exit(1);
		}
		if (first < 1024 || second < 1024){
			ERROR("STUNNEL=> --tunnel cannot be used for privileged ports (< 1024)");
			free(portpairs);
			exit(1);
		}

		if (strcmp(to,"L") == 0 && !port_available(first)){
			ERROR("STUNNEL=> Local port %d is in use or unavailable",first);
			free(portpairs);
			exit(1);
		}
		if (strcmp(to,"R") == 0 && !port_available(second)){
			ERROR("STUNNEL=> Local port %d is in use or unavailable",second);
			free(portpairs);
			exit(1);
		}

		p = strdup(args);
		snprintf(args,256," %s -%s%d:%s:%d ",p,to,first,hoststr,second);
		DEBUG("STUNNEL=> Adding tunnel arg: %s", args);
		free(portpairs[i]);
		free(p);
	}
	free(portpairs);

	DEBUG("STUNNEL=> full args : %s", args);

	// Init STUNNEL_MODE to 1, default is batch mode
	if(args != NULL){
		if (setenv(STUNNEL_MODE, "1", 1) < 0) {
			ERROR("STUNNEL=> Cannot set stunnel mode");
			return -1;
		}
	}
	return (0);
}


/*
 *  Provide a --tunnel option to srun:
 */

struct spank_option spank_opts[] =
{
		{
				"tunnel",
				"<submit port:exec port[,submit port:exec port,...]>",
				"Forward exec host port to submit host port via ssh -L",
				1,
				0,
				(spank_opt_cb_f) _tunnel_opt_process
		},
		SPANK_OPTIONS_TABLE_END
};



/*
 * Process any options on the plugstack.conf line
 */
void _stunnel_init_config(spank_t spank, int ac, char *av[])
{
	DEBUG("STUNNEL=> Parsing config file");
	int i;
	char* elt;
	char* p;

	// get configuration line parameters, replacing '|' with ' '
	for (i = 0; i < ac; i++) {
		elt = av[i];
		if ( strncmp(elt,"ssh_cmd=",8) == 0 ) {
			ssh_cmd=strdup(elt+8);
			p = ssh_cmd;
			while ( p != NULL && *p != '\0' ) {
				if ( *p == '|' )
					*p= ' ';
				p++;
			}
		}
		else if ( strncmp(elt,"ssh_args=",9) == 0 ) {
			ssh_args=strdup(elt+9);
			p = ssh_args;
			while ( p != NULL && *p != '\0' ) {
				if ( *p == '|' )
					*p= ' ';
				p++;
			}
		}
		else if ( strncmp(elt,"helpertask_args=",16) == 0 ) {
			helpertask_args=strdup(elt+16);
			p = helpertask_args;
			while ( p != NULL && *p != '\0' ) {
				if ( *p == '|' )
					*p= ' ';
				p++;
			}
		}
	}
	DEBUG("STUNNEL=> ssh_cmd : %s", ssh_cmd);
	DEBUG("STUNNEL=> ssh_args : %s", ssh_args);
	DEBUG("STUNNEL=> helpertask_args : %s", helpertask_args);

}

/*
 *
 * This is used to process any options in the config file
 *
 */

int slurm_spank_init (spank_t spank, int ac, char *av[])
{
	if (spank_option_register(spank, spank_opts) != ESPANK_SUCCESS) {
		ERROR("STUNNEL=> Cannot register stunnel plugin");
		return (-1);
  }

	_stunnel_init_config(spank, ac, av);
	return 0;
}

/*
 *
 * Build SSH control file
 *
 */
int slurm_spank_init_post_opt (spank_t spank, int ac, char *av[])
{
	// Env are only necessary in remote context to pass to exit
	if (!spank_remote (spank)){
		return 0;
	}

	// Build control file in Remote context with necessary env
	if(args != NULL){
		if(_build_control_file(spank)){
			ERROR("STUNNEL=> Cannot build controlfile name");
			exit(1);
		}
	}

	return 0;
}

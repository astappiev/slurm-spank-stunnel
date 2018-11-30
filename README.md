# slurm-spank-stunnel

`slurm-spank-stunnel` is a [Slurm](http://www.schemd.com/slurm)
[SPANK](https://slurm.schedmd.com/spank.html) plugin that facilitates the
creation of SSH tunnels between submission hosts and compute nodes.

## Description

The goal of `slurm-spank-tunnel` is to allow users to setup port forwarding
during an interactive Slurm session (*srun*) or a batch job (*sbatch/salloc*).
This will be beneficial for IPython notebooks, for instance, but it could
be of use for anything that requires an SSH tunnel.

The general command looks like:

```
$ srun [options] --tunnel=<submit_host_port>:<compute_node_port>[,<submit_host_port>:<compute_node_port>]
```

### Context
* Tunnels started with `srun` are started from the local context  
`submit_host --> compute_node`
* Tunnels started with `sbatch/salloc` are started from the remote context  
from the node starting the task  
`compute_node --> submit_host`

### Examples

#### Interactive session

So for instance, if you want to run an IPython notebook and a Django development
server in the same session, you could start a session like this:

```
$ srun --pty --mem 4000 -p dev --tunnel 8001:8000,8889:8888 bash
```

This will forward:
*  port 8001 on the submission host to port 8000 on the compute node
*  port 8889 on the submission host to port 8888 on the compute node

#### Batch job

If you want to access an external *License_server* inside a batch job going
through the *login_node* you will submit your job like this
```
[myuser@login_node]:$ sbatch --tunnel -L1900:license_server:1900 script
```

If you want to start a IPython notebook inside a batch job and let the script
handles the ssh tunnel, you can submit your job with

```
[myuser@login_node]:$ sbatch --tunnel -R8001:8000 script
```
=> Your notebook will be accessible on the *login_node* on port 8001 (in that
case R is by default so optional).


## Tunnel options

### Direction
You can specify the direction of the tunnel by using the **L** or **R** prefix
like a regular ssh tunnel (*default*: **L** for *srun*/**R** for *sbatch*).
e.g.
```
$ srun --tunnel R8001:8000,L8889:8888
```

### Destination host
Optionally you can specify a destination host for the tunnel, if you do not specify
it, it will default to **localhost**
```
$ sbatch --tunnel L8001:localhost:8000
```

## Configuration
To configure the plugin, add the library to `plugstack.conf` and add optional options if needed (use **|** for spaces):
* **ssh_cmd**: can be used to modify the ssh binary to use  
default corresponds to *ssh_cmd=ssh*
* **ssh_args**: can be used to modify the ssh arguments to use.
default corresponds to *ssh_args=*
* **helpertask_args**: can be used to add a trailing argument to the helper task
responsible for setting up the ssh tunnel  
default corresponds to *helpertask_args=*  
an interesting value can be helpertask_args=2>/tmp/log to
capture the stderr of the helper task

e.g.
```
required	slurm-spank-stunnel.so   ssh_args=-o|StrictHostKeyChecking=no  helpertask_args=>/tmp/stunnel.log|2>&1
```


## Implementation details

All it really does is run an `ssh -L` command while in the "local" Slurm
context (on the submit host) or `ssh -R` command in the "remote" context.
A single command handles the entire list of ports. The `ssh` command is run
using a ControlMaster file, which is used to terminate the connection
after the `srun/sbatch` job is done.

### Functions

`slurm_spank_init()` is run when the `srun` job is initialized and it calls the
option parser. This calls functions that parse the `--tunnel` parameter and
create the `ssh -L` argument. `slurm_spank_init_post_opt()` handles the creation
of the control file once options have been parsed.

`slurm_spank_local_user_init()` is called after `srun` options are processed,
resources are allocated, and a job id is available, but before the job command
is executed. This calls a couple of functions that:
1. get the first node in the list of allocated nodes (hopefully there is just
   one),
2. runs the ssh -L command.

`slurm_spank_task_init()` is called after `sbatch` options are processed,
resources are allocated, and a job id is available, but before the job command
is executed. This calls a couple of functions that:
1. get the submission host
2. runs the ssh -L/R command.

`slurm_spank_exit()` actually gets run when `srun` exits back to the login node.  It checks for the
"host file", named for the user and containing the exec host name, and uses that
to terminate the ssh command via the ControlMaster mechanism.

Because the `slurm_spank_exit()` clears the ssh tunnels by the job user when `srun`
exits and by **root**/the user running `slurmd` on the remote context (this users
needs to have the right to close the tunnel for the user, this is necessary because
there is no callbacks on the remote context started by the user).

The control master is written to /tmp so that the
files are host specific, but that could go in home directories under a
host-specific path.

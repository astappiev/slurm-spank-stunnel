# slurm-spank-stunnel

Slurm SPANK plugin to ease the creation of SSH tunnels and enable port
forwarding to jobs.

## Description

The idea is to allow users to setup port forwarding during an interactive Slurm
session.  This will be beneficial for IPython notebooks, but you can use it for
anything that requires an SSH tunnel.  The command form is:

```
$ srun [options] --tunnel=<submit host port>:<exec host port>[,<submit host port>:<exec host port>]
```

So, if you need to run, say, an IPython notebook and a Django development
server in the same session, but 8000 and 8888 are occupied on the login node,
you'd start a session like this:

```
$ srun --pty --mem 4000 -p interact --tunnel 8001:8000,8889:8888 bash
```

A given user can only do one of these per login host at a time: if you already
have a tunnel setup on `login01`, you can't login to `login01` a second time
and do a new forwarding session.

## Implementation details

All it really does is run an `ssh -L` command while in the "local" Slurm
context (on the submit host).  A single command handles the entire list of
ports.  The `ssh` command is run using a ControlMaster file, which is used to
terminate the connection after the `srun` job is done.

### Functions

`slurm_spank_init()` is run when the `srun` job is initialized and it calls the
option parser.  This calls functions that parse the `--tunnel` parameter and
create the `ssh -L` argument.

`slurm_spank_local_user_init()` is called after `srun` options are processed,
resources are allocated, and a job id is available, but before the job command
is executed.  This calls a couple of functions that:
1. get the first node in the list of allocated nodes (hopefully there is just
   one),
2. runs the ssh -L command.

`slurm_spank_exit()` actually gets run both when `srun` has forked the command
(e.g. `/bin/bash`) and when it exits back to the login node.  It checks for the
"host file", named for the user and containing the exec host name, and uses that
to terminate the ssh command via the ControlMaster mechanism.

Because the `slurm_spank_exit()` function gets called twice (after forking the
command and after exiting the interactive session), there's an exitflag file
that indicates whether you're actually exiting the interactive session.

The hostfile, exitflag, and control master are all written to /tmp so that the
files are host specific, but that could go in home directories under a
host-specific path.

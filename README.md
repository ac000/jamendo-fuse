# jamendo-fuse

This is a FUSE (Filesystem in Userspace) providing access to the jamendo.com
creative commons music platform.

It's pretty basic currently, rather than providing access to every artist on
there, you tell it what artists you want to access.

For example with the following config file (default provided)

```JSON
{
    "artists": [
        [ "peergynt_lobogris",                      "7907" ],
        [ "tunguska_electronic_music_society",      "343607" ]
    ]
}
```

will provide an initial file system with a top level directory of

```
drwxr-xr-x 2 andrew andrew 0 Mar 25 14:25 peergynt_lobogris
drwxr-xr-x 2 andrew andrew 0 Mar 25 14:25 tunguska_electronic_music_society
```

As you move around this filesystem its contents will be dynamically created
from http calls to jamendo.

Under each of the top level artist directories you will have one or more album
directories, under each of those you will find four directories for the four
types of audio available (mp31, mp32, ogg & flac), and under them a directory
of the album tracks. e.g specific track would be at


```
peergynt_lobogris/the_best_of_bluemoons_2009/flac/08_-_cd1_08_always.flac
```

Initial accesses can be a little slow while it does the http requests, but
then this information is cached (except, for now at least, for the actual
track audio data).

As noted above, jamendo provides audio in a number of formats, 96kbit/sec and
VBR MP3, 112kbit/sec Ogg Vorbis and FLAC (around 1Mbit/sec). Those correspond
to the mp31, mp32, ogg & flac above.

# Build

jamendo-fuse has a few dependencies

  - [libac](https://github.com/ac000/libac)
  - [libcurl](https://curl.se/libcurl/)
  - [jansson](https://digip.org/jansson/)
  - [libfuse](https://github.com/libfuse/libfuse) (Version 3)

On Red Hat/Fedora/etc libfuse, libcurl and jansson can be obtained with

```
$ sudo dnf install fuse3 fuse3-devel libcurl{,-devel} jansson{,-devel}
```

The fuse3 package is not required for building, but is needed for mounting the
filesystem.

For libac, you can build an rpm package for it.

First create the rpm-build directory structure

```
$ mkdir -p ~/rpmbuild/{BUILD,RPMS,SOURCES,SPECS,SRPMS}
```

make sure you have the rpm-build package

```
$ sudo dnf install rpm-build
```

Clone libac

```
$ git clone https://github.com/ac000/libac.git
$ cd libac
$ make rpm
$ sudo dnf install ~/rpmbuild/RPMS/x86_64/libac-*
```

and finally jamendo-fuse itself

```
$ cd ..
$ git clone https://github.com/ac000/jamendo-fuse.git
$ cd jamendo-fuse
$ make rpm
$ sudo dnf install ~/rpmbuild/RPMS/x86_64/jamendo-fuse-*
```

# Use

In order to use this, the first thing you will need to do is get a client id
from jamendo. This can be acquired by signing up [here](https://devportal.jamendo.com/signup).

That will generate you a client\_id.

Copy the *artists.json* into *~/.config/jamendo-fuse/*

```
$ mkdir -p ~/.config/jamendo-fuse
$ cp artists.json ~/.config/jamendo-fuse/
```

You need to pass that id into jamendo-fuse. You also need to tell jamendo-fuse
where to mount the filesystem, that can be anywhere you have access to.

So something like this

```
$ JAMENDO_FUSE_CLIENT_ID=<client_id> jamendo-fuse /tmp/jamendo-fuse
```

**NOTE:** The mount point must exist.

You should now be able to browse around that filesystem, find some tracks and
play them just however you normally would.

This can then be unmounted by

```
$ umount /tmp/jamendo-fuse
```

# Config

*artists.json* is a simple config file that lists what artists to present in
the filesystem.

It consists of artists name (normalised) and artist\_id pairs.

The name is how it shows up in the filesystem, all album and track names are
presented in a normalised form by converting to lowercase, and replacing
any characters not [a-z0-9-_.] to a '_'.

The *artist\id* can be found a number of ways, either through the API or by
simply browsing on Jamendo and taking the id out of the url e.g for the
'Tunguska Electronic Music Society' their URL is

```
https://www.jamendo.com/artist/343607/tunguska-electronic-music-society
```

and the artist\_id is '343607'.

# Debugging

You can enable a debug log (/tmp/jamendo-fuse.log) by setting the

```
JAMENDO_FUSE_DEBUG=true
```

environment variable.

You can also build jamendo-fuse with libasan with

```
make ASAN=1
```

Due to libfuse killing off stdio, you will want to have libasan log to a file
instead with

```
ASAN_OPTIONS="log_path=/tmp/asan.log"
```

So you could do something like

```
$ make ASAN=1 && ASAN_OPTIONS="log_path=/tmp/asan.log" JAMENDO_FUSE_DEBUG=true JAMENDO_FUSE_CLIENT_ID=<client_id> ./jamendo-fuse /tmp/jamendo-fuse
```

for testing.

# License

This is licensed under the GNU General Public License (GPL) version 2

See *COPYING* in the repository root for details.

# Contributing

See *CodingStyle.md* & *Contributing.md*

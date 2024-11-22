# jamendo-fuse

This is a FUSE (Filesystem in USErspace) providing access to the jamendo.com
creative commons music platform.

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

jamendo-fuse currently has two modes of operation

1) The original mode whereby a JSON config file is provided describing what
   artists to provide access to.
2) A mode whereby you are able to freely browse by artist.

In order to use either of these modes, the first thing you will need to do
is get a client id from Jamendo. This can be acquired by signing up
[here](https://devportal.jamendo.com/signup).

That will generate you a client\_id.

You can then set this ion your shell, e.g.

```
export JAMENDO_FUSE_CLIENT_ID=client_id
```

or just pass it to jamendo-fuse at runtime, e.g.

```
$ JAMENDO_FUSE_CLIENT_ID=client_id jamendo-fuse ...
```

## Original config file driven mode

In this mode you should provide a JSON file that looks like

```JSON
{
    "artists": [
        [ "peergynt_lobogris",                      "7907" ],
        [ "tunguska_electronic_music_society",      "343607" ]
    ]
}
```

placed under

```
~/.config/jamendo-fuse/artists.json
```

You can use the provided config file as a starting point

```
$ mkdir -p ~/.config/jamendo-fuse
$ cp artists.json ~/.config/jamendo-fuse/
```

You can then start jamendo-fuse like

```
$ jamendo-fuse mountpoint
```

That assumes that you already set *JAMENDO_FUSE_CLIENT_ID* and the
*mountpount* exists.

That will provide an initial filesystem with the root directory containing

```
dr-xr-xr-x 0 andrew andrew 0 Nov 19 03:27 peergynt_lobogris
dr-xr-xr-x 0 andrew andrew 0 Nov 19 03:27 tunguska_electronic_music_society
```

As you move around this filesystem its contents will be dynamically created
from http calls to Jamendo.

Under each of the top level artist directories you will have one or more album
directories, under each of those you will find four directories for the four
types of audio available (mp31, mp32, ogg & flac), and under them a directory
of the album tracks. e.g. specific track would be at

```
peergynt_lobogris/the_best_of_bluemoons_2009/flac/08_-_cd1_08_always.flac
```

When finished you can `killall jamendo-fuse` or you can pas `-f` to
jamendo-fuse which keep it in the foreground and then you can simply ^C it.

Either of those two options will result in the Jamendo-fuse filesystem
being unmounted.

If you run into trouble you can always unmount it manually with

```
$ umount mountpount
```

The *artist\id* can be found a number of ways, either through the API or by
simply browsing on Jamendo and taking the id out of the url e.g for the
'Tunguska Electronic Music Society' their URL is

```
https://www.jamendo.com/artist/343607/tunguska-electronic-music-society
```

and the artist\_id is '343607'.

## Browse mode

In this mode any config file is ignored. This allows you to browse Jamendo
by artist.

You get this mode by passing `--full` to jamendo-fuse e.g.

```
$ jamendo-fuse --full mountpoint
```

In this mode you will have a top-level "artists" (/artists) directory under
which you will have directories a-z and then again under each of those
directories a-z i.e.

```
artists/[a-z]/[a-z]/
```

Under those directories you will then have the artists, and then it's the
same directory structure as above, e.g.

```
artists/p/e/peergynt_lobogris/the_best_of_bluemoons_2009/flac/08_-_cd1_08_always.flac
```

# Names

All artist/album/track names are normalised to only contain the characters

```
[a-z0-9-_.]
```

# Debugging

You can enable debugging by setting the

```
JAMENDO_FUSE_DEBUG=true
```

environment variable.

**NOTE:** For this to work you *must* pass the `'f` command line argument.

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

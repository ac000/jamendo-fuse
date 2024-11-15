/* SPDX-License-Identifier: GPL-2.0 */

/*
 * jamendo-fuse.c - FUSE interface to jamendo.org
 *
 * Copyright (c) 2021		Andrew Clayton <andrew@digital-domain.net>
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <libgen.h>
#include <ctype.h>
#include <limits.h>

#include <curl/curl.h>

#include <jansson.h>

#include <libac.h>

#define FUSE_USE_VERSION 31
#include <fuse.h>

#ifndef gettid
#include <sys/syscall.h>
#define gettid()        syscall(SYS_gettid)
#endif

#define CLIENT_ID		client_id

#define list_foreach(list)	for ( ; list; list = list->next)

#define __unused		__attribute__((unused))

enum {
	ARTIST = 0,
	ALBUM,
	FORMAT,
	TRACK
};

enum {
	FMT_MP31 = 0,
	FMT_MP32,
	FMT_OGG,
	FMT_FLAC,
};

enum file_op {
	FOP_GETATTR = 0,
	FOP_READDIR,
	FOP_READ,
};

struct curl_buf {
	char *buf;
	size_t len;
};

struct jf_file {
	char *name;
	char *date;
	mode_t mode;
	off_t size;
	blkcnt_t blocks;

	char *id;
	char *audio;
	int audio_fmt;
	char *content_type;
};

struct dir_entry {
	char *path;
	int type;
	struct jf_file **jfiles;
};

static const struct audio_fmt {
	const int audio_fmt;
	const char *name;
	const char *ext;
} audio_fmts[] = {
	{ FMT_MP31,	"mp31",		"mp3"	},
	{ FMT_MP32,	"mp32",		"mp3"	},
	{ FMT_OGG,	"ogg",		"oga"	},
	{ FMT_FLAC,	"flac",		"flac"	},
};

static const char *client_id;

static ac_btree_t *fstree;

static bool debug;
static FILE *debug_fp;

#define dbg(fmt, ...) \
	do { \
		if (!debug) \
			break; \
		fprintf(debug_fp, "[%5ld] %s: " fmt, gettid(), __func__, \
			##__VA_ARGS__); \
		fflush(debug_fp); \
	} while (0)

static void free_dentry(void *data)
{
	struct dir_entry *dentry = data;
	int i;

	if (!dentry)
		return;

	for (i = 0; dentry->jfiles[i] != NULL; i++) {
		struct jf_file *jfile = dentry->jfiles[i];

		free(jfile->name);
		free(jfile->id);
		free(jfile->date);
		free(jfile->audio);
		free(jfile->content_type);
		free(jfile);
	}
	free(dentry->jfiles[i]);

	free(dentry->jfiles);
	free(dentry->path);
	free(dentry);
}

static char *normalise_fname(char *name)
{
	size_t len;
	char *ptr = name;

	if (!name)
		return NULL;

	len = strlen(name);
	ptr += len;
	while (*--ptr) {
		switch (*ptr) {
		case 'A' ... 'Z':
			*ptr = tolower(*ptr);
		case 'a' ... 'z':
		case '0' ... '9':
		case '-':
		case '_':
		case '.':
			continue;
		default:
			*ptr = '_';
		}
	}

	return name;
}

static int compare_paths(const void *a, const void *b)
{
	const struct dir_entry *dentry1 = a;
	const struct dir_entry *dentry2 = b;

        return strcmp(dentry1->path, dentry2->path);
}

static size_t header_cb(char *buffer, size_t size, size_t nitems,
			void *userdata)
{
	struct jf_file *jf = userdata;
	char *hdr_val;
	char **ptr;
	size_t ret = nitems * size;

	if (strcasestr(buffer, "Location:"))
		ptr = &jf->audio;
	else
		return ret;

	hdr_val = strchr(buffer, ' ');
	if (!hdr_val)
		return ret;
	free(*ptr);
	*ptr = strdup(ac_str_chomp(++hdr_val));

	return ret;
}

static int curl_get_file_info(struct jf_file *jf)
{
	int ret = 0;
	CURL *curl;
	CURLcode res;
	char *content_type;

	curl = curl_easy_init();

	curl_easy_setopt(curl, CURLOPT_URL, jf->audio);

	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, jf);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
	curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "jamendo-fuse / libcurl");

	res = curl_easy_perform(curl);
	if (res != CURLE_OK) {
		dbg("curl_easy_perform(): %s\n", curl_easy_strerror(res));
		ret = -1;
		goto out_cleanup;
	}

	curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type);
	jf->content_type = strdup(content_type);
	curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &jf->size);

out_cleanup:
	curl_easy_cleanup(curl);

	return ret;
}

static void set_files_format(const char *album_id, const char *path)
{
	struct dir_entry *dentry;
	int n = sizeof(audio_fmts) / sizeof(audio_fmts[0]);

	dentry = calloc(1, sizeof(struct dir_entry));
	dentry->jfiles = calloc(n + 1, sizeof(void *));
	for (int i = 0; i < n; i++) {
		struct jf_file *jf_file;

		jf_file = calloc(1, sizeof(struct jf_file));
		jf_file->name = strdup(audio_fmts[i].name);
		jf_file->mode = 0755 | S_IFDIR;
		jf_file->id = strdup(album_id);
		jf_file->audio_fmt = audio_fmts[i].audio_fmt;

		dentry->jfiles[i] = jf_file;
	}
	dentry->path = strdup(path);
	dentry->type = FORMAT;
	ac_btree_add(fstree, dentry);
}

static void set_files_tracks(const struct curl_buf *buf, const char *ext,
			     const char *path)
{
	json_t *root;
	json_t *results;
	json_t *tracks;
	json_t *trks;
	json_t *track;
	size_t index;
	struct dir_entry *dentry;

	root = json_loads(buf->buf, 0, NULL);
	results = json_object_get(root, "results");
	trks = json_array_get(results, 0);
	tracks = json_object_get(trks, "tracks");

	dentry = calloc(1, sizeof(struct dir_entry));
	dentry->jfiles = calloc(json_array_size(tracks) + 1, sizeof(void *));
	json_array_foreach(tracks, index, track) {
		json_t *id;
		json_t *name;
		json_t *audio;
		json_t *pos;
		int len;
		struct jf_file *jf_file;

		id = json_object_get(track, "id");
		name = json_object_get(track, "name");
		audio = json_object_get(track, "audio");
		pos = json_object_get(track, "position");

		jf_file = calloc(1, sizeof(struct jf_file));

		len = asprintf(&jf_file->name, "%02d_-_%s.%s",
			       atoi(json_string_value(pos)),
			       json_string_value(name), ext);
		if (len == -1) { /* shut GCC up [-Wunused-result] */
			dbg("asprintf() failed!\n");
			jf_file->name = NULL;
		}

		normalise_fname(jf_file->name);
		jf_file->mode = 0644 | S_IFREG;
		jf_file->id = strdup(json_string_value(id));
		jf_file->audio = strdup(json_string_value(audio));

		curl_get_file_info(jf_file);
		jf_file->blocks = (jf_file->size / 512) +
				  (jf_file->size % 512 == 0 ? 0 : 1);

		dentry->jfiles[index] = jf_file;
	}
	dentry->path = strdup(path);
	dentry->type = TRACK;
	ac_btree_add(fstree, dentry);

	json_decref(root);
}

static void set_files_album(const struct curl_buf *buf, const char *path)
{
	json_t *root;
	json_t *albums;
	json_t *album;
	size_t index;
	struct dir_entry *dentry;

	root = json_loads(buf->buf, 0, NULL);
	albums = json_object_get(root, "results");

	dentry = calloc(1, sizeof(struct dir_entry));
	dentry->jfiles = calloc(json_array_size(albums) + 1, sizeof(void *));
	json_array_foreach(albums, index, album) {
		json_t *id;
		json_t *name;
		json_t *date;
		struct jf_file *jf_file;

		id = json_object_get(album, "id");
		name = json_object_get(album, "name");
		date = json_object_get(album, "releasedate");

		jf_file = calloc(1, sizeof(struct jf_file));
		jf_file->name = strdup(json_string_value(name));
		normalise_fname(jf_file->name);
		jf_file->date = strdup(json_string_value(date));
		jf_file->mode = 0755 | S_IFDIR;
		jf_file->id = strdup(json_string_value(id));

		dentry->jfiles[index] = jf_file;
	}
	dentry->path = strdup(path);
	dentry->type = ALBUM;
	ac_btree_add(fstree, dentry);

	json_decref(root);
}

static size_t curl_writeb_cb(void *contents, size_t size, size_t nmemb,
			     void *userp)
{
	size_t realsize = size * nmemb;
	struct curl_buf *curl_buf = userp;

	curl_buf->buf = realloc(curl_buf->buf, curl_buf->len + realsize + 1);

	memcpy(curl_buf->buf + curl_buf->len, contents, realsize);
	curl_buf->len += realsize;
	curl_buf->buf[curl_buf->len] = '\0';

	return realsize;
}

static int curl_perform(const char *url, struct curl_buf *curl_buf)
{
	int ret = 0;
	CURL *curl;
	CURLcode res;

	curl = curl_easy_init();

	curl_easy_setopt(curl, CURLOPT_URL, url);

	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_writeb_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, curl_buf);

	curl_easy_setopt(curl, CURLOPT_USERAGENT, "jamendo-fuse / libcurl");

	res = curl_easy_perform(curl);
	if (res != CURLE_OK) {
		dbg("curl_easy_perform(): %s\n", curl_easy_strerror(res));
		ret = -1;
	}

	curl_easy_cleanup(curl);

	return ret;
}

static ac_slist_t *curls;
/*
 * We _really_ want to use persistent connections when reading the
 * file data. Not doing so introduces too much latency and audio
 * apps suffer buffer drop outs. E.g ogg123 would often get down to
 * a 22% buffer and sometime below and stutter. With this I don't
 * really see it drop below 77%.
 *
 * I'm sure this will also even if very slightly reduce load on the
 * end server.
 *
 * The reason we use thread local storage for this curl handle
 * is due to FUSE being multi-threaded underneath and this seemed
 * like the simplest way to have per thread curl handles.
 */
static __thread CURL *read_file_curl;
static int curl_read_file(const char *url, char *buf, size_t size,
			  off_t offset)
{
	int ret;
	CURLcode res;
	char range[64];
	struct curl_buf curl_buf = {};

	snprintf(range, sizeof(range), "%zu-%zu", offset, offset + size - 1);
	dbg("Requesting bytes [%s] from : %s\n", range, url);

	if (!read_file_curl) {
		read_file_curl = curl_easy_init();
		ac_slist_preadd(&curls, read_file_curl);
		dbg("CURL created new curl handle @ %p\n", read_file_curl);
	}

	curl_easy_setopt(read_file_curl, CURLOPT_URL, url);

	curl_easy_setopt(read_file_curl, CURLOPT_RANGE, range);
	curl_easy_setopt(read_file_curl, CURLOPT_WRITEFUNCTION, curl_writeb_cb);
	curl_easy_setopt(read_file_curl, CURLOPT_WRITEDATA, &curl_buf);

	curl_easy_setopt(read_file_curl, CURLOPT_USERAGENT,
			 "jamendo-fuse / libcurl");

	if (debug) {
		curl_easy_setopt(read_file_curl, CURLOPT_STDERR, debug_fp);
		curl_easy_setopt(read_file_curl, CURLOPT_VERBOSE, 1L);
	}

	res = curl_easy_perform(read_file_curl);
	if (res != CURLE_OK) {
		dbg("CURL curl_easy_perform(): %s\n", curl_easy_strerror(res));
		ret = -1;
		goto out_free;
	}

	if (curl_buf.len > 0)
		memcpy(buf, curl_buf.buf, curl_buf.len);
	ret = curl_buf.len;

out_free:
	free(curl_buf.buf);

	return ret;
}

static void make_full_path(const char *path, const char *name, char *fpath)
{
	if (strcmp(path, "/") == 0)
		snprintf(fpath, PATH_MAX, "/%s", name);
	else
		snprintf(fpath, PATH_MAX, "%s/%s", path, name);
}

static void do_curl(const char *path, int type, const struct jf_file *jfile)
{
	const char *api_fmt = "https://api.jamendo.com/v3.0/albums";
	char api[256];
	struct curl_buf curl_buf = {};

	if (type == ARTIST)
		snprintf(api, sizeof(api),
			 "%s/?client_id=%s&format=json&artist_id=%s&limit=200",
			 api_fmt, CLIENT_ID, jfile->id);
	else if (type == FORMAT)
		snprintf(api, sizeof(api),
			 "%s/tracks/?client_id=%s&format=json&id=%s&audioformat=%s",
			 api_fmt, CLIENT_ID, jfile->id,
			 audio_fmts[jfile->audio_fmt].name);
	else
		return;

	dbg("** api : %s\n", api);
	curl_perform(api, &curl_buf);
	if (type == ARTIST)
		set_files_album(&curl_buf, path);
	else if (type == FORMAT)
		set_files_tracks(&curl_buf,
				 audio_fmts[jfile->audio_fmt].ext,
				 path);

	free(curl_buf.buf);
}

static struct dir_entry *get_dentry(const char *path, enum file_op op)
{
	struct dir_entry *dentry;
	struct dir_entry data;
	char *pathc = NULL;
	char *lpath = NULL;
	bool found = false;
	int i;

	switch (op) {
	case FOP_GETATTR:
	case FOP_READ:
		pathc = strdup(path);
		data.path = dirname(pathc);
		dentry = ac_btree_lookup(fstree, &data);
		if (dentry)
			goto out_free;
		lpath = strdup(data.path);
		data.path = dirname(data.path);
		break;
	case FOP_READDIR:
		data.path = (char *)path;
		dentry = ac_btree_lookup(fstree, &data);
		if (dentry)
			goto out_free;
		pathc = strdup(path);
		lpath = strdup(path);
		data.path = dirname(pathc);
		break;
	}

	dentry = ac_btree_lookup(fstree, &data);
	if (!dentry)
		goto out_free;

	for (i = 0; dentry->jfiles[i] != NULL; i++) {
		char pathname[PATH_MAX];

		make_full_path(dentry->path, dentry->jfiles[i]->name,
			       pathname);
		if (strcmp(lpath, pathname) != 0)
			continue;

		found = true;
		break;
	}
	if (!found) {
		dentry = NULL;
		goto out_free;
	}

	if (dentry->type == ALBUM)
		set_files_format(dentry->jfiles[i]->id, lpath);
	else
		do_curl(lpath, dentry->type, dentry->jfiles[i]);

	data.path = lpath;
	dentry = ac_btree_lookup(fstree, &data);

out_free:
	free(pathc);
	free(lpath);

	return dentry;
}

static int jf_getattr(const char *path, struct stat *st,
		      struct fuse_file_info *fi __unused)
{
	struct dir_entry *dentry;
	bool found = false;
	int i;

	dbg("path [%s]\n", path);

	st->st_uid = getuid();
	st->st_gid = getgid();
	st->st_atime = time(NULL);
	st->st_mtime = time(NULL);

	if (strcmp(path, "/") == 0) {
		st->st_mode = 0755 | S_IFDIR;
		st->st_nlink = 2;
		return 0;
	}

	dentry = get_dentry(path, FOP_GETATTR);
	if (!dentry)
		return -1;

	for (i = 0; dentry->jfiles[i] != NULL; i++) {
		char pathname[PATH_MAX];

		make_full_path(dentry->path, dentry->jfiles[i]->name,
			       pathname);
		if (strcmp(path, pathname) != 0)
			continue;

		found = true;
		break;
	}
	if (!found)
		return -1;

	st->st_mode = dentry->jfiles[i]->mode;
	st->st_nlink = 1;

	if (st->st_mode & S_IFREG) {
		st->st_size = dentry->jfiles[i]->size;
		st->st_blocks = dentry->jfiles[i]->blocks;
	} else if (st->st_mode & S_IFDIR) {
		if (dentry->jfiles[i]->date) {
			struct tm tm = {};
			time_t ds;

			strptime(dentry->jfiles[i]->date, "%F", &tm);
			ds = mktime(&tm);
			st->st_atime = st->st_mtime = ds;
		}

		st->st_nlink++;
	}

	return 0;
}

static int jf_readdir(const char *path, void *buffer, fuse_fill_dir_t filler,
		      off_t offset __unused,
		      struct fuse_file_info *fi __unused,
		      enum fuse_readdir_flags flags __unused)
{
	struct stat sb;
	struct dir_entry *dentry;

	dbg("path [%s]\n", path);

	filler(buffer, ".", NULL, 0, 0);
	filler(buffer, "..", NULL, 0, 0);

	dentry = get_dentry(path, FOP_READDIR);
	if (!dentry)
		return 0;

	for (int i = 0; dentry->jfiles[i] != NULL; i++) {
		memset(&sb, 0, sizeof(struct stat));
		sb.st_mode = dentry->jfiles[i]->mode;
		filler(buffer, dentry->jfiles[i]->name, &sb, 0, 0);
	}

	return 0;
}

static int jf_read(const char *path, char *buffer, size_t size, off_t offset,
		   struct fuse_file_info *fi __unused)
{
	struct dir_entry *dentry;
	bool found = false;
	int i;

	dbg("path [%s]\n", path);

	dentry = get_dentry(path, FOP_READ);
	if (!dentry)
		return -1;

	for (i = 0; dentry->jfiles[i] != NULL; i++) {
		char pathname[PATH_MAX];

		make_full_path(dentry->path, dentry->jfiles[i]->name,
			       pathname);
		if (strcmp(path, pathname) != 0)
			continue;

		found = true;
		break;
	}
	if (!found)
		return -1;

	if (!(offset < dentry->jfiles[i]->size))
		return 0;

	dbg("CURL using curl handle @ %p\n", read_file_curl);

	return curl_read_file(dentry->jfiles[i]->audio, buffer, size, offset);
}

int main(int argc, char *argv[])
{
	json_t *root;
	json_t *artists;
	json_t *artist;
	size_t i;
	const char *dbg;
	char artists_config[PATH_MAX];
	struct dir_entry *dentry;
	const struct fuse_operations jf_operations = {
		.getattr	= jf_getattr,
		.readdir	= jf_readdir,
		.read		= jf_read,
	};

	client_id = getenv("JAMENDO_FUSE_CLIENT_ID");
	if (!client_id || argc < 2) {
		fprintf(stderr,
			"Usage: JAMENDO_FUSE_CLIENT_ID=<client_id> "
			"jamendo-fuse <mount_point>\n");
		exit(EXIT_FAILURE);
	}

	dbg = getenv("JAMENDO_FUSE_DEBUG");
	if (dbg && (*dbg == 'y' || *dbg == 't' || *dbg == '1'))
		debug = true;

	debug_fp = fopen("/tmp/jamendo-fuse.log", "w");

	fstree = ac_btree_new(compare_paths, free_dentry);

	snprintf(artists_config, sizeof(artists_config),
		 "%s/.config/jamendo-fuse/artists.json", getenv("HOME"));
	root = json_load_file(artists_config, 0, NULL);
	if (!root) {
		fprintf(stderr, "Couldn't open artists.json\n");
		exit(EXIT_FAILURE);
	}

	artists = json_object_get(root, "artists");
	dentry = calloc(1, sizeof(struct dir_entry));
	dentry->jfiles = calloc(json_array_size(artists) + 1, sizeof(void *)); 
	json_array_foreach(artists, i, artist) {
		struct jf_file *jf_file;
		json_t *name = json_array_get(artist, 0);
		json_t *id = json_array_get(artist, 1);

		jf_file = calloc(1, sizeof(struct jf_file));
		jf_file->name = strdup(json_string_value(name));
		jf_file->id = strdup(json_string_value(id));
		jf_file->mode = 0755 | S_IFDIR;
		dentry->jfiles[i] = jf_file;
	}
	json_decref(root);
	dentry->path = strdup("/");
	dentry->type = ARTIST;
	ac_btree_add(fstree, dentry);

	curl_global_init(CURL_GLOBAL_DEFAULT);

	printf("jamendo-fuse %s loading.\n", GIT_VERSION);

	fuse_main(argc, argv, &jf_operations, NULL);

	ac_btree_destroy(fstree);
	ac_slist_destroy(&curls, curl_easy_cleanup);
	curl_global_cleanup();
	fclose(debug_fp);

	exit(EXIT_SUCCESS);
}

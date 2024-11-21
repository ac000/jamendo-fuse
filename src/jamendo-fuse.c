/* SPDX-License-Identifier: GPL-2.0 */

/*
 * jamendo-fuse.c - FUSE interface to jamendo.org
 *
 * Copyright (c) 2021 - 2024	Andrew Clayton <andrew@digital-domain.net>
 */

#define _GNU_SOURCE

#include <stddef.h>
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
#include <getopt.h>

#include <curl/curl.h>

#include <jansson.h>

#include <libac.h>

#define FUSE_USE_VERSION 31
#include <fuse.h>

#ifndef gettid
#include <sys/syscall.h>
#define gettid()        syscall(SYS_gettid)
#endif

#define FUSE_MAX_ARGS		8

#define DIR_NLINK_NR		2

#define CLIENT_ID		client_id

#define API_URL_MAX_LEN		256

#define list_foreach(list)	for ( ; list; list = list->next)

#define __unused		__attribute__((unused))

enum getopt_opt_val {
	OPT_FULL = 0,
};

static const struct option long_opts[] = {
	{ "full",	no_argument,		NULL,	OPT_FULL },
	{}
};

enum jf_dentry_type {
	/*
	 * The first four items here *Should* match the items in
	 * jf_autocomplete_entity
	 */
	JF_DT_ARTIST = 0,
	JF_DT_ALBUM,
	JF_DT_TRACK,
	JF_DT_TAG,

	JF_DT_FORMAT,

	JF_DT_TL_ARTISTS,

	JF_DT_TL_A,
	JF_DT_TL_AA,
	JF_DT_TL_AAA,
};

enum jf_autocomplete_entity {
	JF_A_E_ARTIST = 0,
	JF_A_E_ALBUM,
	JF_A_E_TRACK,
	JF_A_E_TAG,
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
	char *orig_name;
	char *name;
	char *date;
	mode_t mode;
	nlink_t nlink;
	off_t size;
	blkcnt_t blocks;

	char *id;
	char *audio;
	int audio_fmt;
	char *content_type;
};

struct dir_entry {
	char *path;
	enum jf_dentry_type type;
	enum jf_autocomplete_entity entity;
	ac_btree_t *jfiles;
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

static const char * const jf_autocomplete_entities[] = {
	[JF_A_E_ARTIST] = "artists",
	[JF_A_E_ALBUM]  = "albums",
	[JF_A_E_TRACK]  = "tracks",
	[JF_A_E_TAG]    = "tags",
};

static const char *client_id;

static size_t nr_root_items = DIR_NLINK_NR;

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

static void free_jf_file(void *data)
{
	struct jf_file *jfile = data;

	if (!jfile)
		return;

	free(jfile->orig_name);
	free(jfile->name);
	free(jfile->id);
	free(jfile->date);
	free(jfile->audio);
	free(jfile->content_type);

	free(jfile);
}

static void free_dentry(void *data)
{
	struct dir_entry *dentry = data;

	if (!dentry)
		return;

	ac_btree_destroy(dentry->jfiles);

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

static int compare_file_paths(const void *a, const void *b)
{
	const struct jf_file *jfile1 = a;
	const struct jf_file *jfile2 = b;

	return strcmp(jfile1->name, jfile2->name);
}

static int compare_dentry_paths(const void *a, const void *b)
{
	const struct dir_entry *dentry1 = a;
	const struct dir_entry *dentry2 = b;

        return strcmp(dentry1->path, dentry2->path);
}

static struct jf_file *lookup_jfile_from_dentry(const char *path,
						const struct dir_entry *dentry)
{
	struct jf_file jfile;

	if (strcmp(path, "/") == 0)
		return NULL;

	jfile.name = strrchr(path, '/') + 1;

	return ac_btree_lookup(dentry->jfiles, &jfile);
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
	size_t n = sizeof(audio_fmts) / sizeof(audio_fmts[0]);

	dentry = calloc(1, sizeof(struct dir_entry));
	dentry->jfiles = ac_btree_new(compare_file_paths, free_jf_file);

	for (size_t i = 0; i < n; i++) {
		struct jf_file *jf_file;

		jf_file = calloc(1, sizeof(struct jf_file));
		jf_file->name = strdup(audio_fmts[i].name);
		jf_file->mode = 0555 | S_IFDIR;
		jf_file->nlink = DIR_NLINK_NR;
		jf_file->id = strdup(album_id);
		jf_file->audio_fmt = audio_fmts[i].audio_fmt;

		ac_btree_add(dentry->jfiles, jf_file);
	}
	dentry->path = strdup(path);
	dentry->type = JF_DT_FORMAT;
	ac_btree_add(fstree, dentry);
}

static void set_files_tracks(const struct curl_buf *buf, const char *ext,
			     const char *path)
{
	json_t *root;
	json_t *results;
	json_t *rdate;
	json_t *tracks;
	json_t *trks;
	json_t *track;
	size_t index;
	struct dir_entry *dentry;

	root = json_loads(buf->buf, 0, NULL);
	results = json_object_get(root, "results");
	trks = json_array_get(results, 0);
	rdate = json_object_get(trks, "releasedate");
	tracks = json_object_get(trks, "tracks");

	dentry = calloc(1, sizeof(struct dir_entry));
	dentry->jfiles = ac_btree_new(compare_file_paths, free_jf_file);

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
		jf_file->mode = 0444 | S_IFREG;
		jf_file->date = strdup(json_string_value(rdate));
		jf_file->id = strdup(json_string_value(id));
		jf_file->audio = strdup(json_string_value(audio));

		curl_get_file_info(jf_file);
		jf_file->blocks = (jf_file->size / 512) +
				  (jf_file->size % 512 == 0 ? 0 : 1);

		ac_btree_add(dentry->jfiles, jf_file);
	}
	dentry->path = strdup(path);
	dentry->type = JF_DT_TRACK;
	ac_btree_add(fstree, dentry);

	json_decref(root);
}

static void set_files_album(const struct curl_buf *buf, const char *path,
			    const struct dir_entry *prev_dir)
{
	json_t *root;
	json_t *albums;
	json_t *album;
	size_t index;
	struct jf_file *jfile;
	struct dir_entry *dentry;
	static const size_t nfmts = sizeof(audio_fmts) / sizeof(audio_fmts[0]);

	root = json_loads(buf->buf, 0, NULL);
	albums = json_object_get(root, "results");

	dentry = calloc(1, sizeof(struct dir_entry));
	dentry->jfiles = ac_btree_new(compare_file_paths, free_jf_file);

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
		jf_file->mode = 0555 | S_IFDIR;
		jf_file->nlink = DIR_NLINK_NR + nfmts;
		jf_file->id = strdup(json_string_value(id));

		ac_btree_add(dentry->jfiles, jf_file);
	}
	dentry->path = strdup(path);
	dentry->type = JF_DT_ALBUM;
	ac_btree_add(fstree, dentry);

	jfile = lookup_jfile_from_dentry(path, prev_dir);
	if (jfile)
		jfile->nlink = DIR_NLINK_NR + index;

	json_decref(root);
}

static void set_file_entity(const struct curl_buf *buf, const char *path,
			    const struct dir_entry *prev_dir)
{
	json_t *root;
	json_t *results;
	json_t *entities;
	struct jf_file *jfile;
	struct dir_entry *dentry;

	root = json_loads(buf->buf, 0, NULL);
	results = json_object_get(root, "results");
	entities = json_object_get(results,
				   jf_autocomplete_entities[prev_dir->entity]);

	dentry = calloc(1, sizeof(struct dir_entry));
	dentry->jfiles = ac_btree_new(compare_file_paths, free_jf_file);

	for (size_t i = 0; i < json_array_size(entities); i++) {
		json_t *entity;
		struct jf_file *jf_file;

		entity = json_array_get(entities, i);

		jf_file = calloc(1, sizeof(struct jf_file));
		jf_file->orig_name = strdup(json_string_value(entity));
		jf_file->name = strdup(json_string_value(entity));
		normalise_fname(jf_file->name);
		jf_file->mode = 0555 | S_IFDIR;

		ac_btree_add(dentry->jfiles, jf_file);
	}
	dentry->path = strdup(path);
	dentry->type = (enum jf_dentry_type)prev_dir->entity;
	ac_btree_add(fstree, dentry);

	jfile = lookup_jfile_from_dentry(path, prev_dir);
	if (jfile)
		jfile->nlink = DIR_NLINK_NR + json_array_size(entities);

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

static char *lookup_artist_id(const char *name)
{
	char api[API_URL_MAX_LEN];
	char *aid = NULL;
	char *cstr;
	CURL *curl;
	json_t *root;
	json_t *results;
	struct curl_buf curl_buf = {};
	static const char *api_fmt =
		"https://api.jamendo.com/v3.0/artists/"
		"?client_id=%s&format=json&name=%s";

	curl = curl_easy_init();
	cstr = curl_easy_escape(curl, name, 0);

	snprintf(api, sizeof(api), api_fmt, CLIENT_ID, cstr);

	dbg("** api : %s\n", api);
	curl_perform(api, &curl_buf);

	curl_free(cstr);
	curl_easy_cleanup(curl);

	root = json_loads(curl_buf.buf, 0, NULL);
	results = json_object_get(root, "results");
	if (json_array_size(results) > 0) {
		json_t *obj;
		json_t *id;

		obj = json_array_get(results, 0);
		id = json_object_get(obj, "id");
		aid = strdup(json_string_value(id));
	}

	json_decref(root);
	free(curl_buf.buf);

	return aid;
}

static void do_curl_autocomplete(const char *path,
				 const struct dir_entry *dentry)
{
	char api[API_URL_MAX_LEN];
	char prefix[4] = {};
	char *ptr;
	struct curl_buf curl_buf = {};
	static const char *api_fmt =
		"https://api.jamendo.com/v3.0/autocomplete/"
		"?client_id=%s&format=json&prefix=%s&entity=%s&limit=200";

	ptr = strchr(path, '/');
	ptr++;
	ptr = strchr(ptr, '/');

	prefix[0] = *++ptr;
	ptr += 2;
	prefix[1] = *ptr;
	ptr += 2;
	prefix[2] = *ptr;

	snprintf(api, sizeof(api), api_fmt, CLIENT_ID, prefix,
		 jf_autocomplete_entities[dentry->entity]);

	dbg("** api : %s\n", api);
	curl_perform(api, &curl_buf);

	set_file_entity(&curl_buf, path, dentry);

	free(curl_buf.buf);
}

static void do_curl(const char *path, const struct dir_entry *dentry,
		    struct jf_file *jfile)
{
	char api[API_URL_MAX_LEN];
	struct curl_buf curl_buf = {};
	const char *api_fmt = "https://api.jamendo.com/v3.0/albums";

	if (dentry->type == JF_DT_ARTIST) {
		if (!jfile->id)
			jfile->id = lookup_artist_id(jfile->orig_name);

		snprintf(api, sizeof(api),
			 "%s/?client_id=%s&format=json&artist_id=%s&limit=200",
			 api_fmt, CLIENT_ID, jfile->id);
	} else if (dentry->type == JF_DT_FORMAT) {
		snprintf(api, sizeof(api),
			 "%s/tracks/?client_id=%s&format=json&id=%s&audioformat=%s",
			 api_fmt, CLIENT_ID, jfile->id,
			 audio_fmts[jfile->audio_fmt].name);
	} else {
		return;
	}

	dbg("** api : %s\n", api);
	curl_perform(api, &curl_buf);
	if (dentry->type == JF_DT_ARTIST)
		set_files_album(&curl_buf, path, dentry);
	else if (dentry->type == JF_DT_FORMAT)
		set_files_tracks(&curl_buf,
				 audio_fmts[jfile->audio_fmt].ext,
				 path);

	free(curl_buf.buf);
}

static void fstree_populate_a_z(const char *path,
				const struct dir_entry *prev_dir)
{
	struct dir_entry *dentry;

	dentry = calloc(1, sizeof(struct dir_entry));
	dentry->jfiles = ac_btree_new(compare_file_paths, free_jf_file);

	for (int c = 'a', i = 0; c <= 'z'; c++, i++) {
		struct jf_file *jf_file;
		char alpha[2];

		jf_file = calloc(1, sizeof(struct jf_file));
		sprintf(alpha, "%c", c);
		jf_file->name = strdup(alpha);
		jf_file->mode = 0555 | S_IFDIR;

		if (prev_dir->type == JF_DT_TL_ARTISTS ||
		    prev_dir->type == JF_DT_TL_A)
			jf_file->nlink = DIR_NLINK_NR + 26;

		ac_btree_add(dentry->jfiles, jf_file);
	}
	dentry->path = strdup(path);
	dentry->entity = prev_dir->entity;

	if (prev_dir->type == JF_DT_TL_ARTISTS)
		dentry->type = JF_DT_TL_A;
	else
		dentry->type = prev_dir->type + 1;

	ac_btree_add(fstree, dentry);
}

static struct dir_entry *get_dentry(const char *path, enum file_op op)
{
	struct jf_file jfile;
	struct jf_file *jfilep;
	struct dir_entry *dentry;
	struct dir_entry data;
	char *pathc = NULL;
	char *lpath = NULL;

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

	jfile.name = strrchr(lpath, '/') + 1;
	jfilep = ac_btree_lookup(dentry->jfiles, &jfile);
	if (!jfilep) {
		dentry = NULL;
		goto out_free;
	}

	switch (dentry->type) {
	case JF_DT_TL_ARTISTS:
	case JF_DT_TL_A ... JF_DT_TL_AA:
		fstree_populate_a_z(lpath, dentry);
		break;
	case JF_DT_TL_AAA:
		do_curl_autocomplete(lpath, dentry);
		break;
	case JF_DT_ALBUM:
		set_files_format(jfilep->id, lpath);
		break;
	default:
		do_curl(lpath, dentry, jfilep);
	}

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
	struct jf_file jfile;
	struct jf_file *jfilep;
	struct dir_entry *dentry;

	dbg("path [%s]\n", path);

	st->st_uid = getuid();
	st->st_gid = getgid();
	st->st_atime = time(NULL);
	st->st_mtime = time(NULL);

	if (strcmp(path, "/") == 0) {
		st->st_mode = 0555 | S_IFDIR;
		st->st_nlink = nr_root_items;
		return 0;
	}

	dentry = get_dentry(path, FOP_GETATTR);
	if (!dentry)
		return -1;

	jfile.name = strrchr(path, '/') + 1;
	jfilep = ac_btree_lookup(dentry->jfiles, &jfile);
	if (!jfilep)
		return -1;

	st->st_mode = jfilep->mode;

	if (st->st_mode & S_IFREG) {
		st->st_size = jfilep->size;
		st->st_blocks = jfilep->blocks;
		st->st_nlink = 1;
	}

	if (st->st_mode & S_IFDIR || dentry->type == JF_DT_TRACK) {
		if (jfilep->date) {
			struct tm tm = {};
			time_t ds;

			strptime(jfilep->date, "%F", &tm);
			ds = mktime(&tm);
			st->st_atime = st->st_mtime = ds;
		}

		if (st->st_mode & S_IFDIR)
			st->st_nlink = jfilep->nlink;
	}

	return 0;
}

struct jf_file_filler_data {
	fuse_fill_dir_t filler;
	void *buffer;
};

static void jf_file_filler(const void *nodep, VISIT which, void *data)
{
	const struct jf_file *jfile = *(struct jf_file **)nodep;
	const struct jf_file_filler_data *jf_data = data;
	struct stat sb = {};

	switch (which) {
	case preorder:
	case endorder:
		return;
	case postorder:
	case leaf:
		sb.st_mode = jfile->mode;
		jf_data->filler(jf_data->buffer, jfile->name, &sb, 0, 0);
	}
}

static int jf_readdir(const char *path, void *buffer, fuse_fill_dir_t filler,
		      off_t offset __unused,
		      struct fuse_file_info *fi __unused,
		      enum fuse_readdir_flags flags __unused)
{
	struct dir_entry *dentry;
	struct jf_file_filler_data jf_data;

	dbg("path [%s]\n", path);

	filler(buffer, ".", NULL, 0, 0);
	filler(buffer, "..", NULL, 0, 0);

	dentry = get_dentry(path, FOP_READDIR);
	if (!dentry)
		return 0;

	jf_data.filler = filler;
	jf_data.buffer = buffer;

	ac_btree_foreach_data(dentry->jfiles, jf_file_filler, &jf_data);

	return 0;
}

static int jf_read(const char *path, char *buffer, size_t size, off_t offset,
		   struct fuse_file_info *fi __unused)
{
	struct jf_file jfile;
	struct jf_file *jfilep;
	struct dir_entry *dentry;

	dbg("path [%s]\n", path);

	dentry = get_dentry(path, FOP_READ);
	if (!dentry)
		return -1;

	jfile.name = strrchr(path, '/') + 1;
	jfilep = ac_btree_lookup(dentry->jfiles, &jfile);
	if (!jfilep)
		return -1;

	if (!(offset < jfilep->size))
		return 0;

	dbg("CURL using curl handle @ %p\n", read_file_curl);

	return curl_read_file(jfilep->audio, buffer, size, offset);
}

static void fstree_init_jamendo(void)
{
	struct jf_file *jf_file;
	struct dir_entry *dentry;

	jf_file = calloc(1, sizeof(struct jf_file));
	jf_file->name = strdup("artists");
	jf_file->mode = 0555 | S_IFDIR;
	jf_file->nlink = DIR_NLINK_NR + 26;

	dentry = calloc(1, sizeof(struct dir_entry));
	dentry->jfiles = ac_btree_new(compare_file_paths, free_jf_file);

	ac_btree_add(dentry->jfiles, jf_file);

	nr_root_items++;

	dentry->path = strdup("/");
	dentry->type = JF_DT_TL_ARTISTS;
	dentry->entity = JF_A_E_ARTIST;
	ac_btree_add(fstree, dentry);
}

static void fstree_init_artists_json(void)
{
	json_t *root;
	json_t *artists;
	json_t *artist;
	size_t i;
	char artists_config[PATH_MAX];
	struct dir_entry *dentry;

	snprintf(artists_config, sizeof(artists_config),
		 "%s/.config/jamendo-fuse/artists.json", getenv("HOME"));
	root = json_load_file(artists_config, 0, NULL);
	if (!root) {
		fprintf(stderr, "Couldn't open artists.json\n");
		exit(EXIT_FAILURE);
	}

	artists = json_object_get(root, "artists");
	dentry = calloc(1, sizeof(struct dir_entry));
	dentry->jfiles = ac_btree_new(compare_file_paths, free_jf_file);

	json_array_foreach(artists, i, artist) {
		struct jf_file *jf_file;
		json_t *name = json_array_get(artist, 0);
		json_t *id = json_array_get(artist, 1);

		jf_file = calloc(1, sizeof(struct jf_file));
		jf_file->name = strdup(json_string_value(name));
		jf_file->id = strdup(json_string_value(id));
		jf_file->mode = 0555 | S_IFDIR;

		ac_btree_add(dentry->jfiles, jf_file);

		nr_root_items++;
	}
	json_decref(root);
	dentry->path = strdup("/");
	dentry->type = JF_DT_ARTIST;
	ac_btree_add(fstree, dentry);
}

static void print_usage(void)
{
	printf("Usage: jamendo-fuse [-f] [--full] mount-point\n");
}

int main(int argc, char *argv[])
{
	int fuse_argc = 0;
	char *fuse_argv[FUSE_MAX_ARGS];
	bool use_config = true;
	const char *dbg;
	static const struct fuse_operations jf_operations = {
		.getattr	= jf_getattr,
		.readdir	= jf_readdir,
		.read		= jf_read,
	};

	client_id = getenv("JAMENDO_FUSE_CLIENT_ID");
	if (!client_id) {
		fprintf(stderr, "JAMENDO_FUSE_CLIENT_ID unset\n");
		exit(EXIT_FAILURE);
	}

	dbg = getenv("JAMENDO_FUSE_DEBUG");
	if (dbg && (*dbg == 'y' || *dbg == 't' || *dbg == '1'))
		debug = true;

	fuse_argv[fuse_argc++] = argv[0];

	while (1) {
		int c;
		int opt_idx = 0;

		c = getopt_long(argc, argv, "f", long_opts, &opt_idx);
		if (c == -1)
			break;

		switch (c) {
		case 'f':
			fuse_argv[fuse_argc++] = "-f";
			break;
		case OPT_FULL:
			use_config = false;
			break;
		default:
			print_usage();
			exit(EXIT_FAILURE);
		}
	}

	if (optind == argc) {
		print_usage();
		exit(EXIT_FAILURE);
	}

	fuse_argv[fuse_argc++] = argv[optind];

	debug_fp = fopen("/tmp/jamendo-fuse.log", "w");

	printf("jamendo-fuse %s loading.\n", GIT_VERSION);

	fstree = ac_btree_new(compare_dentry_paths, free_dentry);

	if (!use_config)
		fstree_init_jamendo();
	else
		fstree_init_artists_json();

	curl_global_init(CURL_GLOBAL_DEFAULT);

	fuse_main(fuse_argc, fuse_argv, &jf_operations, NULL);

	ac_btree_destroy(fstree);
	ac_slist_destroy(&curls, curl_easy_cleanup);
	curl_global_cleanup();
	fclose(debug_fp);

	exit(EXIT_SUCCESS);
}

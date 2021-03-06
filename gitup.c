/*-
 * Copyright (c) 2012-2021, John Mehr <jmehr@umn.edu>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/tree.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/evp.h>
#include <openssl/opensslv.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/ssl3.h>
#include <openssl/err.h>
#include <private/ucl/ucl.h>

#include <ctype.h>
#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <libutil.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

#define	GITUP_VERSION     "0.94"
#define	BUFFER_UNIT_SMALL  4096
#define	BUFFER_UNIT_LARGE  1048576

#ifndef CONFIG_FILE_PATH
#define CONFIG_FILE_PATH "./gitup.conf"
#endif

struct object_node {
	RB_ENTRY(object_node) link;
	char     *hash;
	uint8_t   type;
	uint32_t  index;
	uint32_t  index_delta;
	char     *ref_delta_hash;
	uint32_t  pack_offset;
	char     *buffer;
	uint32_t  buffer_size;
	uint32_t  file_offset;
	bool      can_free;
};

struct file_node {
	RB_ENTRY(file_node) link_hash;
	RB_ENTRY(file_node) link_path;
	mode_t  mode;
	char   *hash;
	char   *path;
	bool    keep;
	bool    save;
};

typedef struct {
	SSL                 *ssl;
	SSL_CTX             *ctx;
	int                  socket_descriptor;
	char                *host;
	char                *host_bracketed;
	uint16_t             port;
	char                *proxy_host;
	uint16_t             proxy_port;
	char                *proxy_username;
	char                *proxy_password;
	char                *proxy_credentials;
	char                *section;
	char                *repository_path;
	char                *branch;
	char                *tag;
	char                *have;
	char                *want;
	char                *response;
	int                  response_blocks;
	uint32_t             response_size;
	bool                 clone;
	bool                 repair;
	struct object_node **object;
	uint32_t             objects;
	char                *pack_data_file;
	char                *path_target;
	char                *path_work;
	char                *remote_data_file;
	char               **ignore;
	int                  ignores;
	bool                 keep_pack_file;
	bool                 use_pack_file;
	int                  verbosity;
	uint8_t              display_depth;
	char                *updating;
	bool                 low_memory;
	int                  back_store;
} connector;

static void     append(char **, unsigned int *, const char *, size_t);
static void     apply_deltas(connector *);
static char *   build_clone_command(connector *);
static char *   build_pull_command(connector *);
static char *   build_repair_command(connector *);
static char *   calculate_file_hash(char *, int);
static char *   calculate_object_hash(char *, uint32_t, int);
static void     connect_server(connector *);
static void     create_tunnel(connector *);
static void     extend_updating_list(connector *, char *);
static void     extract_command_line_want(connector *, char *);
static void     extract_proxy_data(connector *, const char *);
static void     extract_tree_item(struct file_node *, char **);
static void     fetch_pack(connector *, char *);
static int      file_node_compare_hash(const struct file_node *, const struct file_node *);
static int      file_node_compare_path(const struct file_node *, const struct file_node *);
static void     file_node_free(struct file_node *);
static void     get_commit_details(connector *);
static bool     ignore_file(connector *, char *);
static char *   illegible_hash(char *);
static char *   legible_hash(char *);
static void     load_buffer(connector *, struct object_node *);
static int      load_configuration(connector *, const char *, char **, int);
static void     load_file(const char *, char **, uint32_t *);
static void     load_object(connector *, char *, char *);
static void     load_pack(connector *);
static void     load_remote_data(connector *);
static void     make_path(char *, mode_t);
static int      object_node_compare(const struct object_node *, const struct object_node *);
static void     object_node_free(struct object_node *);
static bool     path_exists(const char *);
static void     process_command(connector *, char *);
static void     process_tree(connector *, int, char *, char *);
static void     prune_tree(connector *, char *);
static void     release_buffer(connector *, struct object_node *);
static void     save_file(char *, int, char *, int, int, int);
static void     save_objects(connector *);
static void     save_repairs(connector *);
static void     scan_local_repository(connector *, char *);
static void     send_command(connector *, char *);
static void     setup_ssl(connector *);
static void     store_object(connector *, int, char *, int, int, int, char *);
static char *   trim_path(char *, int, bool *);
static uint32_t unpack_delta_integer(char *, uint32_t *, int);
static void     unpack_objects(connector *);
static uint32_t unpack_variable_length_integer(char *, uint32_t *);
static void     usage(const char *);


/*
 * node_compare
 *
 * Functions that instruct the red-black trees how to sort keys.
 */

static int
file_node_compare_path(const struct file_node *a, const struct file_node *b)
{
	return (strcmp(a->path, b->path));
}


static int
file_node_compare_hash(const struct file_node *a, const struct file_node *b)
{
	return (strcmp(a->hash, b->hash));
}


static int
object_node_compare(const struct object_node *a, const struct object_node *b)
{
	return (strcmp(a->hash, b->hash));
}


/*
 * node_free
 *
 * Functions that free the memory used by tree nodes.
 */

static void
file_node_free(struct file_node *node)
{
	free(node->hash);
	free(node->path);
	free(node);
}


static void
object_node_free(struct object_node *node)
{
	free(node->hash);
	free(node->ref_delta_hash);
	free(node->buffer);
	free(node);
}


static RB_HEAD(Tree_Remote_Path, file_node) Remote_Path = RB_INITIALIZER(&Remote_Path);
RB_PROTOTYPE(Tree_Remote_Path, file_node, link_path, file_node_compare_path)
RB_GENERATE(Tree_Remote_Path,  file_node, link_path, file_node_compare_path)

static RB_HEAD(Tree_Local_Path, file_node) Local_Path = RB_INITIALIZER(&Local_Path);
RB_PROTOTYPE(Tree_Local_Path, file_node, link_path, file_node_compare_path)
RB_GENERATE(Tree_Local_Path,  file_node, link_path, file_node_compare_path)

static RB_HEAD(Tree_Local_Hash, file_node) Local_Hash = RB_INITIALIZER(&Local_Hash);
RB_PROTOTYPE(Tree_Local_Hash, file_node, link_hash, file_node_compare_hash)
RB_GENERATE(Tree_Local_Hash,  file_node, link_hash, file_node_compare_hash)

static RB_HEAD(Tree_Objects, object_node) Objects = RB_INITIALIZER(&Objects);
RB_PROTOTYPE(Tree_Objects, object_node, link, object_node_compare)
RB_GENERATE(Tree_Objects,  object_node, link, object_node_compare)

static RB_HEAD(Tree_Trim_Path, file_node) Trim_Path = RB_INITIALIZER(&Trim_Path);
RB_PROTOTYPE(Tree_Trim_Path, file_node, link_path, file_node_compare_path)
RB_GENERATE(Tree_Trim_Path,  file_node, link_path, file_node_compare_path)


/*
 * release_buffer
 *
 * Function that frees an object buffer.
 */

static void release_buffer(connector *connection, struct object_node *obj)
{
	/* Do not release non file backed objects. */

	if ((connection->low_memory) && (!obj->can_free)) {
		free(obj->buffer);
		obj->buffer = NULL;
	}
}


/*
 * load_buffer
 *
 * Function that loads an object buffer from disk.
 */

static void load_buffer(connector *connection, struct object_node *obj)
{
	int rd;

	if ((connection->low_memory) && (!obj->buffer)) {
		obj->buffer = malloc(obj->buffer_size);

		if (!obj->buffer)
			err(EXIT_FAILURE, "load_buffer: malloc");

		lseek(connection->back_store, obj->file_offset, SEEK_SET);

		rd = read(connection->back_store,
			obj->buffer,
			obj->buffer_size);

		if (rd != (int)obj->buffer_size)
			err(EXIT_FAILURE,
				"load_buffer: read %d != %d",
				rd,
				obj->buffer_size);
	}
}


/*
 * legible_hash
 *
 * Function that converts a 20 byte binary SHA checksum into a 40 byte
 * human-readable SHA checksum.
 */

static char *
legible_hash(char *hash_buffer)
{
	char *hash = NULL;
	int   x = 0;

	if ((hash = (char *)malloc(41)) == NULL)
		err(EXIT_FAILURE, "legible_hash: malloc");

	for (x = 0; x < 20; x++)
		snprintf(&hash[x * 2], 3, "%02x", (unsigned char)hash_buffer[x]);

	hash[40] = '\0';

	return (hash);
}


/*
 * illegible_hash
 *
 * Function that converts a 40 byte human-readable SHA checksum into a 20 byte
 * binary SHA checksum.
 */

static char *
illegible_hash(char *hash_buffer)
{
	char *hash = NULL;
	int   x = 0;

	if ((hash = (char *)malloc(20)) == NULL)
		err(EXIT_FAILURE, "illegible_hash: malloc");

	for (x = 0; x < 20; x++)
		hash[x] = 16 * ((unsigned char)hash_buffer[x * 2] -
			(hash_buffer[x * 2] > 58 ? 87 : 48)) +
			(unsigned char)hash_buffer[x * 2 + 1] -
			(hash_buffer[x * 2 + 1] > 58 ? 87 : 48);

	return (hash);
}


/*
 * ignore_file
 *
 * Return true if path is in the set of "ignores" for the connection.
 */

static bool
ignore_file(connector *connection, char *path)
{
	int x;

	for (x = 0; x < connection->ignores; x++)
		if (strncmp(path, connection->ignore[x], strlen(connection->ignore[x])) == 0)
			return (true);

	return (false);
 }


/*
 * make_path
 *
 * Procedure that creates a directory and all intermediate directories if they
 * do not exist.
 */

static void
make_path(char *path, mode_t mode)
{
	char *temp = path;

	if (mkdir(path, mode) == -1) {
		if ((errno != ENOENT) && (errno != EEXIST))
			err(EXIT_FAILURE, "make_path: cannot create %s", path);

		/* Create any missing intermediate directories. */

		while ((temp = strchr(temp, '/')) != NULL) {
			if (temp != path) {
				*temp = '\0';

				if (!path_exists(path))
					if ((mkdir(path, mode) == -1) && (errno != EEXIST))
						err(EXIT_FAILURE,
							"make_path: cannot create %s",
							path);

				*temp = '/';
			}

			temp++;
		}

	/* Create the target directory. */

	if ((mkdir(path, mode) == -1) && (errno != EEXIST))
		err(EXIT_FAILURE, "make_path: cannot create %s", path);
	}
}


/*
 * prune_tree
 *
 * Procedure that recursively removes a directory.
 */

static void
prune_tree(connector *connection, char *base_path)
{
	DIR           *directory = NULL;
	struct dirent *entry = NULL;
	struct stat    sb;
	char           full_path[strlen(base_path) + 1 + MAXNAMLEN + 1];

	/* Sanity check the directory to prune. */

	if (strnstr(base_path, connection->path_target, strlen(connection->path_target)) != base_path)
		errc(EXIT_FAILURE, EACCES,
			"prune_tree: %s is not located in the %s tree",
			base_path,
			connection->path_target);

	if (strnstr(base_path, "../", strlen(base_path)) != NULL)
		errc(EXIT_FAILURE, EACCES,
			"prune_tree: illegal path traverse in %s",
			base_path);

	/* Remove the directory contents. */

	if ((directory = opendir(base_path)) == NULL)
		return;

	while ((entry = readdir(directory)) != NULL) {
		snprintf(full_path, sizeof(full_path),
			"%s/%s",
			base_path,
			entry->d_name);

		if (stat(full_path, &sb) != 0)
			err(EXIT_FAILURE,
				"prune_tree: cannot stat() %s",
				full_path);

		if (S_ISDIR(sb.st_mode) != 0) {
			if ((entry->d_namlen == 1) && (strcmp(entry->d_name, "." ) == 0))
				continue;

			if ((entry->d_namlen == 2) && (strcmp(entry->d_name, "..") == 0))
				continue;

			prune_tree(connection, full_path);
		} else {
			remove(full_path);
		}
	}

	closedir(directory);

	if (rmdir(base_path) != 0)
		fprintf(stderr,
			" ! cannot remove %s\n",
			base_path);
}


/*
 * path_exists
 *
 * Function wrapper for stat that checks to see if a path exists.
 */

static bool
path_exists(const char *path)
{
	struct stat check;

	return (stat(path, &check) == 0 ? true : false);

}


/*
 * load_file
 *
 * Procedure that loads a local file into the specified buffer.
 */

static void
load_file(const char *path, char **buffer, uint32_t *buffer_size)
{
	struct stat file;
	int         fd;

	if (stat(path, &file) == -1)
		err(EXIT_FAILURE, "load_file: cannot find %s", path);

	if (file.st_size > 0) {
		if (file.st_size > *buffer_size) {
			*buffer_size = file.st_size;

			if ((*buffer = (char *)realloc(*buffer, *buffer_size + 1)) == NULL)
				err(EXIT_FAILURE, "load_file: malloc");
		}

		if ((fd = open(path, O_RDONLY)) == -1)
			err(EXIT_FAILURE, "load_file: cannot read %s", path);

		if ((uint32_t)read(fd, *buffer, *buffer_size) != *buffer_size)
			err(EXIT_FAILURE, "load_file: problem reading %s", path);

		close(fd);

		*(*buffer + *buffer_size) = '\0';
	}
}


/*
 * trim_path
 *
 * Procedure that trims a path to the specified display depth.
 */

static char *
trim_path(char *path, int display_depth, bool *just_added)
{
	struct file_node *new_node = NULL, find;
	int               x = -1;
	char             *trim = NULL, *trimmed_path = NULL;

	trimmed_path = strdup(path);

	if (display_depth == 0)
		return (trimmed_path);

	trim = trimmed_path;

	while ((x++ < display_depth) && (trim != NULL))
		trim = strchr(trim + 1, '/');

	if (trim)
		*trim = '\0';

	find.path = trimmed_path;

	if (!RB_FIND(Tree_Trim_Path, &Trim_Path, &find)) {
		new_node = (struct file_node *)malloc(sizeof(struct file_node));

		if (new_node == NULL)
			err(EXIT_FAILURE, "trim_path: malloc");

		new_node->path = strdup(trimmed_path);
		new_node->mode = 0;
		new_node->hash = NULL;
		new_node->save = 0;

		RB_INSERT(Tree_Trim_Path, &Trim_Path, new_node);
		*just_added = true;
	} else {
		*just_added = false;
	}

	return (trimmed_path);
}


/*
 * save_file
 *
 * Procedure that saves a blob/file.
 */

static void
save_file(char *path, int mode, char *buffer, int buffer_size, int verbosity, int display_depth)
{
	char temp_buffer[buffer_size + 1], *trim = NULL, *display_path = NULL;
	int  fd;
	bool exists = false, just_added = false;

	display_path = trim_path(path, display_depth, &just_added);

	if (display_depth > 0)
		exists |= path_exists(display_path);

	/* Create the directory, if needed. */

	if ((trim = strrchr(path, '/')) != NULL) {
		*trim = '\0';

		if (!path_exists(path))
			make_path(path, 0755);

		*trim = '/';
	}

	/* Print the file or trimmed path. */

	if (verbosity > 0) {
		exists |= path_exists(path);

		if ((display_depth == 0) || (just_added))
			printf(" %c %s\n", (exists ? '*' : '+'), display_path);
	}

	free(display_path);

	if (S_ISLNK(mode)) {
		/*
		 * Make sure the buffer is null terminated, then save it as the
		 * file to link to.
		 */

		memcpy(temp_buffer, buffer, buffer_size);
		temp_buffer[buffer_size] = '\0';

		if (symlink(temp_buffer, path) == -1)
			err(EXIT_FAILURE,
				"save_file: symlink failure %s -> %s",
				path,
				temp_buffer);
	} else {
		/* If the file exists, make sure the permissions are intact. */

		if (path_exists(path))
			chmod(path, mode);

		fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);

		if ((fd == -1) && (errno != EEXIST))
			err(EXIT_FAILURE,
				"save_file: write file failure %s",
				path);

		chmod(path, mode);
		write(fd, buffer, buffer_size);
		close(fd);
	}
}


/*
 * calculate_object_hash
 *
 * Function that adds Git's "type file-size\0" header to a buffer and returns
 * the SHA checksum.
 */

static char *
calculate_object_hash(char *buffer, uint32_t buffer_size, int type)
{
	int         digits = buffer_size, header_width = 0;
	char       *hash = NULL, *hash_buffer = NULL, *temp_buffer = NULL;
	const char *types[8] = { "", "commit", "tree", "blob", "tag", "", "ofs-delta", "ref-delta" };

	if ((hash_buffer = (char *)malloc(20)) == NULL)
		err(EXIT_FAILURE, "calculate_object_hash: malloc");

	if ((temp_buffer = (char *)malloc(buffer_size + 24)) == NULL)
		err(EXIT_FAILURE, "calculate_object_hash: malloc");

	/* Start with the git "type file-size\0" header. */

	header_width = strlen(types[type]) + 3;

	while ((digits /= 10) > 0)
		header_width++;

	snprintf(temp_buffer, header_width, "%s %u", types[type], buffer_size);

	/* Then add the buffer. */

	memcpy(temp_buffer + header_width, buffer, buffer_size);

	/* Calculate the SHA checksum. */

	SHA1((uint8_t *)temp_buffer, buffer_size + header_width, (uint8_t *)hash_buffer);

	hash = legible_hash(hash_buffer);

	free(hash_buffer);
	free(temp_buffer);

	return (hash);
}


/*
 * calculate_file_hash
 *
 * Function that loads a local file and returns the SHA checksum.
 */

static char *
calculate_file_hash(char *path, int file_mode)
{
	char     *buffer = NULL, *hash = NULL, temp_path[BUFFER_UNIT_SMALL];
	uint32_t  buffer_size = 0, bytes_read = 0;

	if (S_ISLNK(file_mode)) {
		bytes_read = readlink(path, temp_path, BUFFER_UNIT_SMALL);
		temp_path[bytes_read] = '\0';

		hash = calculate_object_hash(temp_path, strlen(temp_path), 3);
	} else {
		load_file(path, &buffer, &buffer_size);
		hash = calculate_object_hash(buffer, buffer_size, 3);
		free(buffer);
	}

	return (hash);
}


/*
 * append
 *
 * Procedure that appends one string to another.
 */

static void
append(char **buffer, unsigned int *buffer_size, const char *addendum, size_t addendum_size)
{
	*buffer = (char *)realloc(*buffer, *buffer_size + addendum_size + 1);

	if (*buffer == NULL)
		err(EXIT_FAILURE, "append: realloc");

	memcpy(*buffer + *buffer_size, addendum, addendum_size);
	*buffer_size += addendum_size;
	*(*buffer + *buffer_size) = '\0';
}


/*
 * extend_updating_list
 *
 * Procedure that adds the path of an UPDATING file to a string.
 */

static void
extend_updating_list(connector *connection, char *path)
{
	unsigned int size;
	char         temp[BUFFER_UNIT_SMALL];

	size = (connection->updating ? strlen(connection->updating) : 0);

	snprintf(temp, sizeof(temp), "#\t%s\n", path);
	append(&connection->updating, &size, temp, strlen(temp));
}


/*
 * load_remote_data
 *
 * Procedure that loads the list of remote data and checksums, if it exists.
 */

static void
load_remote_data(connector *connection)
{
	struct file_node *file = NULL;
	char     *buffer = NULL, *hash = NULL, *temp_hash = NULL;
	char     *line = NULL, *raw = NULL, *path = NULL, *data = NULL;
	char      temp[BUFFER_UNIT_SMALL], base_path[BUFFER_UNIT_SMALL];
	char      item[BUFFER_UNIT_SMALL];
	uint32_t  count = 0, data_size = 0, buffer_size = 0, item_length = 0;

	load_file(connection->remote_data_file, &data, &data_size);
	raw = data;

	while ((line = strsep(&raw, "\n"))) {
		/* The first line stores the "have". */

		if (count++ == 0) {
			connection->have = strdup(line);
			continue;
		}

		/*
		 * Empty lines signify the end of a directory entry, so create
		 * an obj_tree for what has been read.
		 */

		if (strlen(line) == 0) {
			if (buffer != NULL) {
				if (connection->clone == false)
					store_object(connection,
						2,
						buffer,
						buffer_size,
						0,
						0,
						NULL);

				buffer = NULL;
				buffer_size = 0;
			}

			continue;
		}

		/* Split the line. */

		hash = strchr(line,     '\t');
		path = strchr(hash + 1, '\t');

		if ((hash == NULL) || (path == NULL)) {
			fprintf(stderr,
				" ! Malformed line '%s' in %s.  Skipping...\n",
				line,
				connection->remote_data_file);

			continue;
		} else {
			hash++;
			path++;
		}

		*(hash -  1) = '\0';
		*(hash + 40) = '\0';

		/* Store the file data. */

		file = (struct file_node *)malloc(sizeof(struct file_node));

		if (file == NULL)
			err(EXIT_FAILURE, "load_remote_data: malloc");

		file->mode = strtol(line, (char **)NULL, 8);
		file->hash = strdup(hash);
		file->save = 0;

		if (path[strlen(path) - 1] == '/') {
			snprintf(base_path, sizeof(base_path), "%s", path);
			snprintf(temp, sizeof(temp), "%s", path);
			temp[strlen(path) - 1] = '\0';
		} else {
			snprintf(temp, sizeof(temp), "%s%s", base_path, path);

			temp_hash = illegible_hash(hash);

			/*
			 * Build the item and add it to the buffer that will
			 * become the obj_tree for this directory.
			 */

			snprintf(item, sizeof(item) - 22, "%s %s", line, path);
			item_length = strlen(item);
			memcpy(item + item_length + 1, temp_hash, 20);
			item_length += 21;
			item[item_length] = '\0';

			append(&buffer, &buffer_size, item, item_length);

			free(temp_hash);
		}

		file->path = strdup(temp);

		RB_INSERT(Tree_Remote_Path, &Remote_Path, file);
	}

	free(data);
}


/*
 * scan_local_repository
 *
 * Procedure that recursively finds and adds local files and directories to
 * separate red-black trees.
 */

static void
scan_local_repository(connector *connection, char *base_path)
{
	DIR              *directory = NULL;
	struct stat       file;
	struct dirent    *entry = NULL;
	struct file_node *new_node = NULL, find, *found = NULL;
	char             *full_path = NULL, file_hash[20];
	int               full_path_size = 0;

	/* Make sure the base path exists in the remote data list. */

	find.path = base_path;
	found     = RB_FIND(Tree_Remote_Path, &Remote_Path, &find);

	/* Add the base path to the local trees. */

	if ((new_node = (struct file_node *)malloc(sizeof(struct file_node))) == NULL)
		err(EXIT_FAILURE, "scan_local_repository: malloc");

	new_node->mode = (found ? found->mode : 040000);
	new_node->hash = (found ? strdup(found->hash) : NULL);
	new_node->path = strdup(base_path);
	new_node->keep = (strlen(base_path) == strlen(connection->path_target) ? true : false);
	new_node->save = false;

	RB_INSERT(Tree_Local_Path, &Local_Path, new_node);

	if (found)
		RB_INSERT(Tree_Local_Hash, &Local_Hash, new_node);

	/* Process the directory's contents. */

	if ((stat(base_path, &file) != -1) && ((directory = opendir(base_path)) != NULL)) {
		while ((entry = readdir(directory)) != NULL) {
			if ((entry->d_namlen == 1) && (strcmp(entry->d_name, "." ) == 0))
				continue;

			if ((entry->d_namlen == 2) && (strcmp(entry->d_name, "..") == 0))
				continue;

			if ((entry->d_namlen == 4) && (strcmp(entry->d_name, ".git") == 0)) {
				fprintf(stderr,
					" ! A .git directory was found -- "
					"gitup does not update this directory "
					"which will cause problems for the "
					"official Git client.\n ! If you wish "
					"to use gitup, please remove %s/%s and "
					"rerun gitup.\n",
					base_path,
					entry->d_name);

				exit(EXIT_FAILURE);
				}

			full_path_size = strlen(base_path) + entry->d_namlen + 2;
			full_path      = (char *)malloc(full_path_size + 1);

			if (full_path == NULL)
				err(EXIT_FAILURE,
					"scan_local_repository: malloc");

			snprintf(full_path,
				full_path_size,
				"%s/%s",
				base_path,
				entry->d_name);

			if (lstat(full_path, &file) == -1)
				err(EXIT_FAILURE,
					"scan_local_repository: cannot read %s",
					full_path);

			if (S_ISDIR(file.st_mode)) {
				scan_local_repository(connection, full_path);
				free(full_path);
			} else {
				if ((new_node = (struct file_node *)malloc(sizeof(struct file_node))) == NULL)
					err(EXIT_FAILURE,
						"scan_local_repository: malloc");

				new_node->mode = file.st_mode;
				new_node->path = full_path;
				new_node->keep = (strnstr(full_path, ".gituprevision", strlen(full_path)) != NULL ? true : false);
				new_node->save = false;

				if (ignore_file(connection, full_path)) {
					new_node->hash = (char *)malloc(20);

					if (new_node->hash == NULL)
						err(EXIT_FAILURE,
							"scan_local_repository: malloc");

					SHA1((uint8_t *)full_path, strlen(full_path), (uint8_t *)file_hash);
					memcpy(new_node->hash, file_hash, 20);
				} else
					new_node->hash = calculate_file_hash(full_path, file.st_mode);

				RB_INSERT(Tree_Local_Hash, &Local_Hash, new_node);
				RB_INSERT(Tree_Local_Path, &Local_Path, new_node);
			}
		}

		closedir(directory);
	}
}


/*
 * load_object
 *
 * Procedure that loads a local file and adds it to the array/tree of pack
 * file objects.
 */

static void
load_object(connector *connection, char *hash, char *path)
{
	struct object_node *object = NULL, lookup_object;
	struct file_node   *find = NULL, lookup_file;
	char               *buffer = NULL;
	uint32_t            buffer_size = 0;

	lookup_object.hash = hash;
	lookup_file.hash   = hash;
	lookup_file.path   = path;

	/*
	 * If the object doesn't exist, look for it first by hash, then by path
	 * and if it is found and the SHA checksum references a file, load it
	 * and store it.
	 */

	if ((object = RB_FIND(Tree_Objects, &Objects, &lookup_object)) != NULL)
		return;

	find = RB_FIND(Tree_Local_Hash, &Local_Hash, &lookup_file);

	if (find == NULL)
		find = RB_FIND(Tree_Local_Path, &Local_Path, &lookup_file);

	if (find) {
		if (!S_ISDIR(find->mode)) {
			load_file(find->path, &buffer, &buffer_size);

			store_object(connection,
				3,
				buffer,
				buffer_size,
				0,
				0,
				NULL);
		}
	} else {
		errc(EXIT_FAILURE, ENOENT,
			"load_object: local file for object %s -- %s not found",
			hash,
			path);
	}
}


/*
 * create_tunnel
 *
 * Procedure that sends a CONNECT command to create a proxy tunnel.
 */

static void
create_tunnel(connector *connection)
{
	char command[BUFFER_UNIT_SMALL];

	snprintf(command,
		BUFFER_UNIT_SMALL,
		"CONNECT %s:%d HTTP/1.1\r\n"
		"Host: %s:%d\r\n"
		"%s"
		"\r\n",
		connection->host_bracketed,
		connection->port,
		connection->host_bracketed,
		connection->port,
		connection->proxy_credentials);

		process_command(connection, command);
}


/*
 * connect_server
 *
 * Procedure that (re)establishes a connection with the server.
 */

static void
connect_server(connector *connection)
{
	struct addrinfo hints, *start, *temp;
	struct timeval  timeout;
	int             error = 0, option = 1;
	char            type[10];
	char           *host = (connection->proxy_host ? connection->proxy_host : connection->host);

	snprintf(type, sizeof(type), "%d", (connection->proxy_host ? connection->proxy_port : connection->port));

	bzero(&hints, sizeof(struct addrinfo));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	if ((error = getaddrinfo(host, type, &hints, &start)))
		errx(EXIT_FAILURE, "%s", gai_strerror(error));

	for (temp = start; temp != NULL; temp = temp->ai_next) {
		if ((connection->socket_descriptor = socket(temp->ai_family, temp->ai_socktype, temp->ai_protocol)) < 0)
			/* trying each addr returned, cont. with next in list */
			continue;

		if (connect(connection->socket_descriptor, temp->ai_addr, temp->ai_addrlen) != -1)
			break;

		close(connection->socket_descriptor);
	}

	freeaddrinfo(start);

	if (temp == NULL)
		err(EXIT_FAILURE,
			"connect_server: connect failure (%d)",
			errno);

	if (setsockopt(connection->socket_descriptor, SOL_SOCKET, SO_KEEPALIVE, &option, sizeof(int)))
		err(EXIT_FAILURE,
			"setup_ssl: setsockopt SO_KEEPALIVE error");

	option = BUFFER_UNIT_LARGE;

	if (setsockopt(connection->socket_descriptor, SOL_SOCKET, SO_SNDBUF, &option, sizeof(int)))
		err(EXIT_FAILURE,
			"setup_ssl: setsockopt SO_SNDBUF error");

	if (setsockopt(connection->socket_descriptor, SOL_SOCKET, SO_RCVBUF, &option, sizeof(int)))
		err(EXIT_FAILURE,
			"setup_ssl: setsockopt SO_RCVBUF error");

	bzero(&timeout, sizeof(struct timeval));
	timeout.tv_sec = 300;

	if (setsockopt(connection->socket_descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(struct timeval)))
		err(EXIT_FAILURE,
			"setup_ssl: setsockopt SO_RCVTIMEO error");

	if (setsockopt(connection->socket_descriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(struct timeval)))
		err(EXIT_FAILURE,
			"setup_ssl: setsockopt SO_SNDTIMEO error");
}


/*
 * setup_ssl
 *
 * Procedure that sends a command to the server and processes the response.
 */

static void
setup_ssl(connector *connection)
{
	int error = 0;

	SSL_library_init();
	SSL_load_error_strings();
	connection->ctx = SSL_CTX_new(SSLv23_client_method());
	SSL_CTX_set_mode(connection->ctx, SSL_MODE_AUTO_RETRY);
	SSL_CTX_set_options(connection->ctx, SSL_OP_ALL | SSL_OP_NO_TICKET);

	if ((connection->ssl = SSL_new(connection->ctx)) == NULL)
		err(EXIT_FAILURE, "setup_ssl: SSL_new");

	SSL_set_fd(connection->ssl, connection->socket_descriptor);

	while ((error = SSL_connect(connection->ssl)) == -1)
		fprintf(stderr,
			"setup_ssl: SSL_connect error: %d\n",
			SSL_get_error(connection->ssl, error));
}


/*
 * process_command
 *
 * Procedure that sends a command to the server and processes the response.
 */

static void
process_command(connector *connection, char *command)
{
	char  read_buffer[BUFFER_UNIT_SMALL];
	char *marker_start = NULL, *marker_end = NULL, *data_start = NULL;
	int   chunk_size = -1, bytes_expected = 0;
	int   marker_offset = 0, data_start_offset = 0;
	int   bytes_read = 0, total_bytes_read = 0, bytes_to_move = 0;
	int   bytes_sent = 0, total_bytes_sent = 0, check_bytes = 0;
	int   bytes_to_write = 0, response_code = 0, error = 0, outlen = 0;
	bool  ok = false, chunked_transfer = true;
	char *temp = NULL;

	bytes_to_write = strlen(command);

	if (connection->verbosity > 1)
		fprintf(stderr, "%s\n\n", command);

	/* Transmit the command to the server. */

	while (total_bytes_sent < bytes_to_write) {
		if (connection->ssl)
			bytes_sent = SSL_write(
				connection->ssl,
				command + total_bytes_sent,
				bytes_to_write - total_bytes_sent);
		else
			bytes_sent = write(
				connection->socket_descriptor,
				command + total_bytes_sent,
				bytes_to_write - total_bytes_sent);

		if (bytes_sent <= 0) {
			if ((bytes_sent < 0) && ((errno == EINTR) || (errno == 0)))
				continue;
			else
				err(EXIT_FAILURE, "process_command: send");
		}

		total_bytes_sent += bytes_sent;

		if (connection->verbosity > 1)
			fprintf(stderr,
				"\r==> bytes sent: %d",
				total_bytes_sent);
	}

	if (connection->verbosity > 1)
		fprintf(stderr, "\n");

	/* Process the response. */

	while (chunk_size) {
		if (connection->ssl)
			bytes_read = SSL_read(
				connection->ssl,
				read_buffer,
				BUFFER_UNIT_SMALL);
		else
			bytes_read = read(
				connection->socket_descriptor,
				read_buffer,
				BUFFER_UNIT_SMALL);

		if (bytes_read == 0)
			break;

		if (bytes_read < 0)
			err(EXIT_FAILURE,
				"process_command: SSL_read error: %d",
				SSL_get_error(connection->ssl, error));

		/*
		 * Expand the buffer if needed, preserving the position and
		 * data_start if the buffer moves.
		 */

		if (total_bytes_read + bytes_read + 1 > connection->response_blocks * BUFFER_UNIT_LARGE) {
			marker_offset     = marker_start - connection->response;
			data_start_offset = data_start   - connection->response;

			if ((connection->response = (char *)realloc(connection->response, ++connection->response_blocks * BUFFER_UNIT_LARGE)) == NULL)
				err(EXIT_FAILURE, "process_command: realloc");

			marker_start = connection->response + marker_offset;
			data_start   = connection->response + data_start_offset;
		}

		/* Add the bytes received to the buffer. */

		memcpy(connection->response + total_bytes_read, read_buffer, bytes_read);
		total_bytes_read += bytes_read;
		connection->response[total_bytes_read] = '\0';

		if (connection->verbosity > 1)
			fprintf(stderr, "\r==> "
				"bytes read: %d\t"
				"bytes_expected: %d\t"
				"total_bytes_read: %d",
				bytes_read,
				bytes_expected,
				total_bytes_read);

		while ((connection->verbosity == 1) && (isatty(STDERR_FILENO))) {
			struct timespec now;
			static struct timespec then;
			char buf[80];
			char htotalb[7];
			char persec[8];
			static int last_total;
			static double sum;
			double secs;
			int64_t throughput;

			if (clock_gettime(CLOCK_MONOTONIC_FAST, &now) == -1)
				err(EXIT_FAILURE,
					"process_command: clock_gettime");

			if (then.tv_sec == 0)
				then = now, secs = 1, sum = 1;
			else {
				secs = now.tv_sec - then.tv_sec +
					(now.tv_nsec - then.tv_nsec) * 1e-9;

				if (1 > secs)
					break;
				else
					sum += secs;
			}

			throughput = ((total_bytes_read - last_total) / secs);

			humanize_number(htotalb, sizeof(htotalb),
				(int64_t)total_bytes_read,
				"B",
				HN_AUTOSCALE,
				HN_DECIMAL | HN_DIVISOR_1000);

			humanize_number(persec, sizeof(persec),
				throughput,
				"B",
				HN_AUTOSCALE,
				HN_DECIMAL | HN_DIVISOR_1000);

			snprintf(buf, sizeof(buf) - 1,
				"  %s in %dm%02ds, %s/s now",
				htotalb,
				(int)(sum / 60),
				(int)sum % 60,
				persec);

			if (isatty(STDERR_FILENO))
				outlen = fprintf(stderr, "%-*s\r", outlen, buf) - 1;

			last_total = total_bytes_read;
			then = now;
			break;
		}

		/* Find the boundary between the header and the data. */

		if (chunk_size == -1) {
			if ((marker_start = strnstr(connection->response, "\r\n\r\n", total_bytes_read)) == NULL) {
				continue;
			} else {
				bytes_expected = marker_start - connection->response + 4;
				marker_start += 2;
				data_start = marker_start;

				/* Check the response code. */

				if (strstr(connection->response, "HTTP/1.") == connection->response) {
					response_code = strtol(strchr(connection->response, ' ') + 1, (char **)NULL, 10);

					if (response_code == 200)
						ok = true;

					if ((connection->proxy_host) && (response_code >= 200) && (response_code < 300))
						ok = true;
				}

				temp = strstr(connection->response, "Content-Length: ");

				if (temp != NULL) {
					bytes_expected += strtol(temp + 16, (char **)NULL, 10);
					chunk_size = -2;
					chunked_transfer = false;
				}
			}
		}

		/* Successful CONNECT responses do not contain a body. */

		if ((strstr(command, "CONNECT ") == command) && (ok))
			break;

		if ((!chunked_transfer) && (total_bytes_read < bytes_expected))
			continue;

		while ((chunked_transfer) && (total_bytes_read + chunk_size > bytes_expected)) {
			/* Make sure the whole chunk marker has been read. */

			check_bytes = total_bytes_read - (marker_start + 2 - connection->response);

			if (check_bytes < 0)
				break;

			marker_end = strnstr(marker_start + 2, "\r\n", check_bytes);

			if (marker_end == NULL)
				break;

			/* Remove the chunk length marker. */

			chunk_size    = strtol(marker_start, (char **)NULL, 16);
			bytes_to_move = total_bytes_read - (marker_end + 2 - connection->response) + 1;

			if (bytes_to_move < 0)
				break;

			memmove(marker_start, marker_end + 2, bytes_to_move);
			total_bytes_read -= (marker_end + 2 - marker_start);

			if (chunk_size == 0)
				break;

			marker_start += chunk_size;
			bytes_expected += chunk_size;
		}
	}

	if ((connection->verbosity) && (isatty(STDERR_FILENO)))
		fprintf(stderr, "\r\e[0K\r");

	if (!ok)
		errc(EXIT_FAILURE, EINVAL,
			"process_command: read failure:\n%s\n",
			connection->response);

	/* Remove the header. */

	connection->response_size = total_bytes_read - (data_start - connection->response);
	memmove(connection->response, data_start, connection->response_size);
	connection->response[connection->response_size] = '\0';
}


/*
 * send_command
 *
 * Function that constructs the command to the fetch the full pack data.
 */

static void
send_command(connector *connection, char *want)
{
	char   *command = NULL;
	size_t  want_size = 0;

	want_size = strlen(want);

	if ((command = (char *)malloc(BUFFER_UNIT_SMALL + want_size)) == NULL)
		err(EXIT_FAILURE, "send_command: malloc");

	snprintf(command,
		BUFFER_UNIT_SMALL + want_size,
		"POST %s/git-upload-pack HTTP/1.1\r\n"
		"Host: %s:%d\r\n"
		"User-Agent: gitup/%s\r\n"
		"Accept-encoding: deflate, gzip\r\n"
		"Content-type: application/x-git-upload-pack-request\r\n"
		"Accept: application/x-git-upload-pack-result\r\n"
		"Git-Protocol: version=2\r\n"
		"Content-length: %zu\r\n"
		"\r\n"
		"%s",
		connection->repository_path,
		connection->host_bracketed,
		connection->port,
		GITUP_VERSION,
		want_size,
		want);

	process_command(connection, command);

	free(command);
}


/*
 * build_clone_command
 *
 * Function that constructs and executes the command to the fetch the shallow
 * pack data.
 */

static char *
build_clone_command(connector *connection)
{
	char *command = NULL;

	if ((command = (char *)malloc(BUFFER_UNIT_SMALL)) == NULL)
		err(EXIT_FAILURE, "build_clone_command: malloc");

	snprintf(command,
		BUFFER_UNIT_SMALL,
		"0011command=fetch0001"
		"000fno-progress"
		"000dofs-delta"
		"0034shallow %s"
		"0032want %s\n"
		"0009done\n0000",
		connection->want,
		connection->want);

	return (command);
}


/*
 * build_pull_command
 *
 * Function that constructs and executes the command to the fetch the
 * incremental pack data.
 */

static char *
build_pull_command(connector *connection)
{
	char *command = NULL;

	if ((command = (char *)malloc(BUFFER_UNIT_SMALL)) == NULL)
		err(EXIT_FAILURE, "build_pull_command: malloc");

	snprintf(command,
		BUFFER_UNIT_SMALL,
		"0011command=fetch0001"
		"000dthin-pack"
		"000fno-progress"
		"000dofs-delta"
		"0034shallow %s"
		"0034shallow %s"
		"000cdeepen 1"
		"0032want %s\n"
		"0032have %s\n"
		"0009done\n0000",
		connection->want,
		connection->have,
		connection->want,
		connection->have);

	return (command);
}


/*
 * build_repair_command
 *
 * Procedure that compares the local repository tree with the data saved from
 * the last run to see if anything has been modified.
 */

static char *
build_repair_command(connector *connection)
{
	struct file_node *find = NULL, *found = NULL;
	char             *command = NULL, *want = NULL, line[BUFFER_UNIT_SMALL];
	const char       *message[2] = { "is missing.", "has been modified." };
	uint32_t          want_size = 0;

	RB_FOREACH(find, Tree_Remote_Path, &Remote_Path) {
		found = RB_FIND(Tree_Local_Path, &Local_Path, find);

		if ((found == NULL) || ((strncmp(found->hash, find->hash, 40) != 0) && (!ignore_file(connection, find->path)))) {
			if (connection->verbosity)
				fprintf(stderr,
					" ! %s %s\n",
					find->path,
					message[found ? 1 : 0]);

			snprintf(line, sizeof(line),
				"0032want %s\n",
				find->hash);

			append(&want, &want_size, line, strlen(line));
		}
	}

	if (want_size == 0)
		return (NULL);

	if (want_size > 3276800)
		errc(EXIT_FAILURE, E2BIG,
			"build_repair_command: There are too many files to "
			"repair -- please re-clone the repository");

	if ((command = (char *)malloc(BUFFER_UNIT_SMALL + want_size)) == NULL)
		err(EXIT_FAILURE, "build_repair_command: malloc");

	snprintf(command,
		BUFFER_UNIT_SMALL + want_size,
		"0011command=fetch0001"
		"000dthin-pack"
		"000fno-progress"
		"000dofs-delta"
		"%s"
		"000cdeepen 1"
		"0009done\n0000",
		want);

	return (command);
}


/*
 * get_commit_details
 */

static void
get_commit_details(connector *connection)
{
	char       command[BUFFER_UNIT_SMALL], ref[BUFFER_UNIT_SMALL], want[41];
	char       peeled[BUFFER_UNIT_SMALL];
	char      *position = NULL;
	time_t     current;
	struct tm  now;
	int        tries = 2, year = 0, quarter = 0;
	uint16_t   length = 0;
	bool       detached = (connection->want != NULL ? true : false);

	/* Send the initial info/refs command. */

	snprintf(command,
		BUFFER_UNIT_SMALL,
		"GET %s/info/refs?service=git-upload-pack HTTP/1.1\r\n"
		"Host: %s:%d\r\n"
		"User-Agent: gitup/%s\r\n"
		"Git-Protocol: version=2\r\n"
		"\r\n",
		connection->repository_path,
		connection->host_bracketed,
		connection->port,
		GITUP_VERSION);

	process_command(connection, command);

	if (connection->verbosity > 1)
		printf("%s\n", connection->response);

	/* Make sure the server supports the version 2 protocol. */

	if (strnstr(connection->response, "version 2", connection->response_size) == NULL)
		errc(EXIT_FAILURE, EPROTONOSUPPORT,
			"%s does not support the version 2 wire protocol",
			connection->host);

	/* Fetch the list of refs. */

	snprintf(command,
		BUFFER_UNIT_SMALL,
		"0014command=ls-refs\n"
		"0016object-format=sha1"
		"0001"
		"0009peel\n"
		"000csymrefs\n"
		"0014ref-prefix HEAD\n"
		"001bref-prefix refs/heads/\n"
		"001aref-prefix refs/tags/\n"
		"0000");

	send_command(connection, command);

	if (connection->verbosity > 1)
		printf("%s\n", connection->response);

	/* Extract the "want" checksum. */

	want[0] = '\0';

	while ((tries-- > 0) && (want[0] == '\0') && (detached == false)) {
		if (strncmp(connection->branch, "quarterly", 9) == 0) {
			/*
			 * If the current calendar quarter doesn't exist, try
			 * the previous one.
			 */

			current = time(NULL);
			now     = *localtime(&current);
			year    = 1900 + now.tm_year + ((tries == 0) && (now.tm_mon < 3) ? -1 : 0);
			quarter = ((now.tm_mon / 3) + (tries == 0 ? 3 : 0)) % 4 + 1;

			snprintf(ref, BUFFER_UNIT_SMALL, " refs/heads/%04dQ%d", year, quarter);
		} else if (connection->tag != NULL) {
			snprintf(ref, BUFFER_UNIT_SMALL, " refs/tags/%s", connection->tag);
		} else {
			snprintf(ref, BUFFER_UNIT_SMALL, " refs/heads/%s", connection->branch);
		}

		/*
		 * Look for the "want" in peeled references first, then look
		 * before the ref.
		 */

		snprintf(peeled, sizeof(peeled), "%s peeled:", ref);

		if ((position = strstr(connection->response, peeled)) != NULL)
			memcpy(want, position + strlen(peeled), 40);
		else if ((position = strstr(connection->response, ref)) != NULL)
			memcpy(want, position - 40, 40);
		else if (tries == 0)
			errc(EXIT_FAILURE, EINVAL,
				"get_commit_details:%s doesn't exist in %s",
				ref,
				connection->repository_path);
	}

	/* Retain the name of the quarterly branch being used. */

	if (strncmp(connection->branch, "quarterly", 9) == 0) {
		free(connection->branch);
		connection->branch = strdup(ref + 12);
	}

	if (want[0] != '\0') {
		if (connection->want == NULL)
			if ((connection->want = (char *)malloc(41)) == NULL)
				err(EXIT_FAILURE, "get_commit_details: malloc");

		memcpy(connection->want, want, 40);
		connection->want[40] = '\0';

		if (connection->verbosity)
			fprintf(stderr, "# Want: %s\n", connection->want);
	}

	/*
	 * Because there is no way to lookup commit history, if a want commit
	 * is specified, change the branch to (detached).
	 */

	if (detached == true) {
		free(connection->branch);
		connection->branch = strdup("(detached)");
	}

	if ((connection->verbosity) && (connection->tag == NULL))
		fprintf(stderr, "# Branch: %s\n", connection->branch);

	/* Create the pack file name. */

	if (connection->keep_pack_file == true) {
		length = strlen(connection->section) + 47;

		connection->pack_data_file = (char *)malloc(length + 1);

		if (connection->pack_data_file == NULL)
			err(EXIT_FAILURE, "get_commit_details: malloc");

		snprintf(connection->pack_data_file, length,
			"%s-%s.pack",
			connection->section,
			connection->want);

		if (connection->verbosity)
			fprintf(stderr,
				"# Saving pack file: %s\n",
				connection->pack_data_file);
	}
}


/*
 * load_pack
 *
 * Procedure that loads a local copy of the pack data or fetches it from
 * the server.
 */

static void
load_pack(connector *connection)
{
	char hash[20];
	int  pack_size = 0;

	load_file(connection->pack_data_file,
		&connection->response,
		&connection->response_size);

	pack_size = connection->response_size - 20;

	/* Verify the pack data checksum. */

	SHA1((uint8_t *)connection->response, pack_size, (uint8_t *)hash);

	if (memcmp(connection->response + pack_size, hash, 20) != 0)
		errc(EXIT_FAILURE, EAUTH,
			"load_pack: pack checksum mismatch -- "
			"expected: %s, received: %s",
			legible_hash(connection->response + pack_size),
			legible_hash(hash));

	/* Process the pack data. */

	unpack_objects(connection);

	free(connection->response);
	connection->response        = NULL;
	connection->response_size   = 0;
	connection->response_blocks = 0;
}


/*
 * fetch_pack
 *
 * Procedure that fetches pack data from the server.
 */

static void
fetch_pack(connector *connection, char *command)
{
	char *pack_start = NULL, hash[20];
	int   chunk_size = 1, pack_size = 0, source = 0, target = 0;

	/* Request the pack data. */

	send_command(connection, command);

	/* Find the start of the pack data and remove the header. */

	if ((pack_start = strstr(connection->response, "PACK")) == NULL)
		errc(EXIT_FAILURE, EFTYPE,
			"fetch_pack: malformed pack data:\n%s",
			connection->response);

	pack_start -= 5;
	connection->response_size -= (pack_start - connection->response + 11);
	memmove(connection->response, connection->response + 8, 4);

	/* Remove the chunk size markers from the pack data. */

	source = pack_start - connection->response;

	while (chunk_size > 0) {
		chunk_size = strtol(connection->response + source, (char **)NULL, 16);

		if (chunk_size == 0)
			break;

		memmove(connection->response + target,
			connection->response + source + 5,
			chunk_size - 5);

		target += chunk_size - 5;
		source += chunk_size;
		connection->response_size -= 5;
	}

	connection->response_size += 5;
	pack_size = connection->response_size - 20;

	/* Verify the pack data checksum. */

	SHA1((uint8_t *)connection->response, pack_size, (uint8_t *)hash);

	if (memcmp(connection->response + pack_size, hash, 20) != 0)
		errc(EXIT_FAILURE, EAUTH,
			"fetch_pack: pack checksum mismatch -- "
			"expected: %s, received: %s",
			legible_hash(connection->response + pack_size),
			legible_hash(hash));

	/* Save the pack data. */

	if (connection->keep_pack_file == true)
		save_file(connection->pack_data_file,
			0644,
			connection->response,
			connection->response_size,
			0,
			0);

	/* Process the pack data. */

	unpack_objects(connection);

	free(command);
}


/*
 * store_object
 *
 * Procedure that creates a new object and stores it in the array and
 * lookup tree.
 */

static void
store_object(connector *connection, int type, char *buffer, int buffer_size, int pack_offset, int index_delta, char *ref_delta_hash)
{
	struct object_node *object = NULL, find;
	char               *hash = NULL;

	hash = calculate_object_hash(buffer, buffer_size, type);

	/* Check to make sure the object doesn't already exist. */

	find.hash = hash;
	object    = RB_FIND(Tree_Objects, &Objects, &find);

	if ((object != NULL) && (connection->repair == false)) {
		free(hash);
	} else {
		/* Extend the array if needed, create a new node and add it. */

		if (connection->objects % BUFFER_UNIT_SMALL == 0)
			if ((connection->object = (struct object_node **)realloc(connection->object, (connection->objects + BUFFER_UNIT_SMALL) * sizeof(struct object_node *))) == NULL)
				err(EXIT_FAILURE, "store_object: realloc");

		object = (struct object_node *)malloc(sizeof(struct object_node));

		if (object == NULL)
			err(EXIT_FAILURE, "store_object: malloc");

		object->index          = connection->objects;
		object->type           = type;
		object->hash           = hash;
		object->pack_offset    = pack_offset;
		object->index_delta    = index_delta;
		object->ref_delta_hash = (ref_delta_hash ? legible_hash(ref_delta_hash) : NULL);
		object->buffer         = buffer;
		object->buffer_size    = buffer_size;
		object->can_free       = true;
		object->file_offset    = -1;

		if (connection->verbosity > 1)
			fprintf(stdout,
				"###### %05d-%d\t%d\t%u\t%s\t%d\t%s\n",
				object->index,
				object->type,
				object->pack_offset,
				object->buffer_size,
				object->hash,
				object->index_delta,
				object->ref_delta_hash);

		if (type < 6)
			RB_INSERT(Tree_Objects, &Objects, object);

		connection->object[connection->objects++] = object;
	}
}


/*
 * unpack_objects
 *
 * Procedure that extracts all of the objects from the pack data.
 */

static void
unpack_objects(connector *connection)
{
	int            buffer_size = 0, total_objects = 0, object_type = 0;
	int            index_delta = 0, stream_code = 0, version = 0;
	int            stream_bytes = 0, x = 0, tot_len = 0;
	char          *buffer = NULL, *ref_delta_hash = NULL;
	char           remote_files_tmp[BUFFER_UNIT_SMALL];
	uint32_t       file_size = 0, file_bits = 0, pack_offset = 0;
	uint32_t       lookup_offset = 0, position = 4, nobj_old = 0;
	unsigned char  zlib_out[16384];

	/* Setup the temporary object store file. */

	if (connection->low_memory) {
		snprintf(remote_files_tmp, BUFFER_UNIT_SMALL,
			"%s.tmp",
			connection->remote_data_file);

		connection->back_store = open(remote_files_tmp,
			O_WRONLY | O_CREAT | O_TRUNC);

		if (connection->back_store == -1)
			err(EXIT_FAILURE,
				"unpack_objects: object file write failure %s",
				remote_files_tmp);

		chmod(remote_files_tmp, 0644);
	}

	/* Check the pack version number. */

	version   = (unsigned char)connection->response[position + 3];
	position += 4;

	if (version != 2)
		errc(EXIT_FAILURE, EFTYPE,
			"unpack_objects: pack version %d not supported",
			version);

	/* Determine the number of objects in the pack data. */

	for (x = 0; x < 4; x++, position++)
		total_objects = (total_objects << 8) + (unsigned char)connection->response[position];

	if (connection->verbosity > 1)
		fprintf(stderr,
			"\npack version: %d, total_objects: %d, pack_size: %d\n\n",
			version,
			total_objects,
			connection->response_size);

	/* Unpack the objects. */

	while ((position < connection->response_size) && (total_objects-- > 0)) {
		object_type    = (unsigned char)connection->response[position] >> 4 & 0x07;
		pack_offset    = position;
		index_delta    = 0;
		file_size      = 0;
		stream_bytes   = 0;
		ref_delta_hash = NULL;

		/* Extract the file size. */

		do {
			file_bits  = connection->response[position] & (stream_bytes == 0 ? 0x0F : 0x7F);
			file_size += (stream_bytes == 0 ? file_bits : file_bits << (4 + 7 * (stream_bytes - 1)));
			stream_bytes++;
		}
		while (connection->response[position++] & 0x80);

		/* Find the object->index referred to by the ofs-delta. */

		if (object_type == 6) {
			lookup_offset = 0;
			index_delta   = connection->objects;

			do lookup_offset = (lookup_offset << 7) + (connection->response[position] & 0x7F) + 1;
			while (connection->response[position++] & 0x80);

			while (--index_delta > 0)
				if (pack_offset - lookup_offset + 1 == connection->object[index_delta]->pack_offset)
					break;

			if (index_delta == 0)
				errc(EXIT_FAILURE, EINVAL,
					"unpack_objects: cannot find ofs-delta "
					"base object");
		}

		/* Extract the ref-delta checksum. */

		if (object_type == 7) {
			if ((ref_delta_hash = (char *)malloc(20)) == NULL)
				err(EXIT_FAILURE, "unpack_objects: malloc");

			memcpy(ref_delta_hash, connection->response + position, 20);
			position += 20;
		}

		/* Inflate and store the object. */

		buffer      = NULL;
		buffer_size = 0;

		z_stream stream = {
			.zalloc   = Z_NULL,
			.zfree    = Z_NULL,
			.opaque   = Z_NULL,
			.avail_in = connection->response_size - position,
			.next_in  = (unsigned char *)(connection->response + position),
			};

		stream_code = inflateInit(&stream);

		if (stream_code == Z_DATA_ERROR)
			errc(EXIT_FAILURE, EILSEQ,
				"unpack_objects: zlib data stream failure");

		do {
			stream.avail_out = 16384,
			stream.next_out  = zlib_out;
			stream_code      = inflate(&stream, Z_NO_FLUSH);
			stream_bytes     = 16384 - stream.avail_out;

			if ((buffer = (char *)realloc(buffer, buffer_size + stream_bytes)) == NULL)
				err(EXIT_FAILURE, "unpack_objects: realloc");

			memcpy(buffer + buffer_size, zlib_out, stream_bytes);
			buffer_size += stream_bytes;
		}
		while (stream.avail_out == 0);

		inflateEnd(&stream);
		position += stream.total_in;

		if (connection->low_memory) {
			write(connection->back_store, buffer, buffer_size);
			nobj_old = connection->objects;
		}

		store_object(connection,
			object_type,
			buffer,
			buffer_size,
			pack_offset,
			index_delta,
			ref_delta_hash);

		if (connection->low_memory) {
			if (nobj_old != connection->objects) {
				connection->object[nobj_old]->buffer      = NULL;
				connection->object[nobj_old]->can_free    = false;
				connection->object[nobj_old]->file_offset = tot_len;
			}

			tot_len += buffer_size;

			free(buffer);
		}

		free(ref_delta_hash);
	}

	if (connection->low_memory) {
		close(connection->back_store);

		/* Reopen the temporary object store file for reading. */

		connection->back_store = open(remote_files_tmp, O_RDONLY);

		if (connection->back_store == -1)
			err(EXIT_FAILURE,
				"unpack_objects: open tmp ro failure %s",
				remote_files_tmp);

		unlink(remote_files_tmp);   /* unlink now / dealocate when exit */
	}
}


/*
 * unpack_delta_integer
 *
 * Function that reconstructs a 32 bit integer from the data stream.
 */

static uint32_t
unpack_delta_integer(char *data, uint32_t *position, int bits)
{
	uint32_t result = 0, read_bytes = 0, temp = 0, mask = 8;

	/* Determine how many bytes in the stream are needed. */

	do if (bits & mask) read_bytes++;
	while (mask >>= 1);

	/*
	 * Put the bytes in their proper position based on the bit field
	 * passed in.
	 */

	if (read_bytes > 0) {
		temp = read_bytes;
		mask = 3;

		do {
			if (bits & (1 << mask))
				result += ((unsigned char)data[*position + --temp] << (mask * 8));
		}
		while (mask-- > 0);

		*position += read_bytes;
	}

	return (result);
}


/*
 * unpack_variable_length_integer
 *
 * Function that reconstructs a variable length integer from the data stream.
 */

static uint32_t
unpack_variable_length_integer(char *data, uint32_t *position)
{
	uint32_t result = 0, count = 0;

	do result += (data[*position] & 0x7F) << (7 * count++);
	while (data[(*position)++] & 0x80);

	return (result);
}


/*
 * apply_deltas
 *
 * Procedure that applies the changes in all of the delta objects to their
 * base objects.
 */

static void
apply_deltas(connector *connection)
{
	struct object_node *delta, *base, lookup;
	int       x = 0, instruction = 0, length_bits = 0, offset_bits = 0;
	int       o = 0, delta_count = -1, deltas[BUFFER_UNIT_SMALL];
	char     *start, *merge_buffer = NULL, *layer_buffer = NULL;
	uint32_t  offset = 0, position = 0, length = 0;
	uint32_t  layer_buffer_size = 0, merge_buffer_size = 0;
	uint32_t  old_file_size = 0, new_file_size = 0, new_position = 0;

	for (o = connection->objects - 1; o >= 0; o--) {
		merge_buffer = NULL;
		delta        = connection->object[o];
		delta_count  = 0;

		if (delta->type < 6)
			continue;

		/* Follow the chain of ofs-deltas down to the base object. */

		while (delta->type == 6) {
			deltas[delta_count++] = delta->index;
			delta = connection->object[delta->index_delta];
			lookup.hash = delta->hash;
		}

		/* Find the ref-delta base object. */

		if (delta->type == 7) {
			deltas[delta_count++] = delta->index;
			lookup.hash = delta->ref_delta_hash;
			load_object(connection, lookup.hash, NULL);
		}

		/* Lookup the base object and setup the merge buffer. */

		if ((base = RB_FIND(Tree_Objects, &Objects, &lookup)) == NULL)
			errc(EXIT_FAILURE, ENOENT,
				"apply_deltas: cannot find %05d -> %d/%s",
				delta->index,
				delta->index_delta,
				delta->ref_delta_hash);

		if ((merge_buffer = (char *)malloc(base->buffer_size)) == NULL)
			err(EXIT_FAILURE,
				"apply_deltas: malloc");

		load_buffer(connection, base);

		memcpy(merge_buffer, base->buffer, base->buffer_size);
		merge_buffer_size = base->buffer_size;

		/* Loop though the deltas to be applied. */

		for (x = delta_count - 1; x >= 0; x--) {
			delta = connection->object[deltas[x]];
			load_buffer(connection, delta);

			position      = 0;
			new_position  = 0;
			old_file_size = unpack_variable_length_integer(delta->buffer, &position);
			new_file_size = unpack_variable_length_integer(delta->buffer, &position);

			/* Make sure the layer buffer is large enough. */

			if (new_file_size > layer_buffer_size) {
				layer_buffer_size = new_file_size;

				if ((layer_buffer = (char *)realloc(layer_buffer, layer_buffer_size)) == NULL)
					err(EXIT_FAILURE, "apply_deltas: realloc");
			}

			/* Loop through the copy/insert instructions and build up the layer buffer. */

			while (position < delta->buffer_size) {
				instruction = (unsigned char)delta->buffer[position++];

				if (instruction & 0x80) {
					length_bits = (instruction & 0x70) >> 4;
					offset_bits = (instruction & 0x0F);

					offset = unpack_delta_integer(delta->buffer, &position, offset_bits);
					start  = merge_buffer + offset;
					length = unpack_delta_integer(delta->buffer, &position, length_bits);

					if (length == 0)
						length = 65536;
				} else {
					offset    = position;
					start     = delta->buffer + offset;
					length    = instruction;
					position += length;
				}

				if (new_position + length > new_file_size)
					errc(EXIT_FAILURE, ERANGE,
						"apply_deltas: position overflow -- %u + %u > %u",
						new_position,
						length,
						new_file_size);

				memcpy(layer_buffer + new_position, start, length);
				new_position += length;
			}

			/* Make sure the merge buffer is large enough. */

			if (new_file_size > merge_buffer_size) {
				merge_buffer_size = new_file_size;
				merge_buffer = (char *)realloc(merge_buffer, merge_buffer_size);

				if (merge_buffer == NULL)
					err(EXIT_FAILURE,
						"apply_deltas: realloc");
			}

			/*
			 * Store the layer buffer in the merge buffer for the
			 * next loop iteration.
			 */

			memcpy(merge_buffer, layer_buffer, new_file_size);
			release_buffer(connection, delta);
		}

		/* Store the completed object. */

		release_buffer(connection, base);

		store_object(connection,
			base->type,
			merge_buffer,
			new_file_size,
			0,
			0,
			NULL);
	}
}


/*
 * extract_tree_item
 *
 * Procedure that extracts mode/path/hash items in a tree and returns them in
 * a new file_node.
 */

static void
extract_tree_item(struct file_node *file, char **position)
{
	int x = 0, path_size = 0;

	/* Extract the file mode. */

	file->mode = strtol(*position, (char **)NULL, 8);
	*position  = strchr(*position, ' ') + 1;

	/* Extract the file path. */

	path_size = strlen(*position) + 1;
	snprintf(file->path, path_size, "%s", *position);
	*position += path_size;

	/* Extract the file SHA checksum. */

	for (x = 0; x < 20; x++)
		snprintf(&file->hash[x * 2],
			3,
			"%02x",
			(uint8_t)*(*position)++);

	file->hash[40] = '\0';
}


/*
 * save_tree
 *
 * Procedure that processes all of the obj-trees and retains the current files.
 */

static void
process_tree(connector *connection, int remote_descriptor, char *hash, char *base_path)
{
	struct object_node  object, *found_object = NULL, *tree = NULL;
	struct file_node    file, *found_file = NULL;
	struct file_node   *new_file_node = NULL, *remote_file = NULL;
	char                full_path[BUFFER_UNIT_SMALL], *buffer = NULL;
	char                line[BUFFER_UNIT_SMALL], *position = NULL;
	unsigned int        buffer_size = 0;

	object.hash = hash;

	if ((tree = RB_FIND(Tree_Objects, &Objects, &object)) == NULL)
		errc(EXIT_FAILURE, ENOENT,
			"process_tree: tree %s -- %s cannot be found",
			base_path,
			object.hash);

	/* Remove the base path from the list of upcoming deletions. */

	load_buffer(connection, tree);

	file.path  = base_path;
	found_file = RB_FIND(Tree_Local_Path, &Local_Path, &file);

	if (found_file != NULL) {
		found_file->keep = true;
		found_file->save = false;
	}

	/* Add the base path to the output. */

	if ((file.path = (char *)malloc(BUFFER_UNIT_SMALL)) == NULL)
		err(EXIT_FAILURE, "process_tree: malloc");

	if ((file.hash = (char *)malloc(41)) == NULL)
		err(EXIT_FAILURE, "process_tree: malloc");

	snprintf(line, sizeof(line),
		"%o\t%s\t%s/\n",
		040000,
		hash,
		base_path);

	append(&buffer, &buffer_size, line, strlen(line));

	/* Process the tree items. */

	position = tree->buffer;

	while ((uint32_t)(position - tree->buffer) < tree->buffer_size) {
		extract_tree_item(&file, &position);

		snprintf(full_path, sizeof(full_path),
			"%s/%s",
			base_path,
			file.path);

		snprintf(line, sizeof(line),
			"%o\t%s\t%s\n",
			file.mode,
			file.hash,
			file.path);

		append(&buffer, &buffer_size, line, strlen(line));

		/* Recursively walk the trees and process the files/links. */

		if (S_ISDIR(file.mode)) {
			process_tree(connection, remote_descriptor, file.hash, full_path);
		} else {
			/*
			 * Locate the pack file object and local copy of
			 * the file.
			 */

			memcpy(object.hash, file.hash, 41);
			memcpy(file.path, full_path, strlen(full_path) + 1);

			found_object = RB_FIND(Tree_Objects, &Objects, &object);
			found_file   = RB_FIND(Tree_Local_Path, &Local_Path, &file);

			/* If the local file hasn't changed, skip it. */

			if (found_file != NULL) {
				found_file->keep = true;
				found_file->save = false;

				if (strncmp(file.hash, found_file->hash, 40) == 0)
					continue;
			}

			/*
			 * Missing objects can sometimes be found by searching
			 * the local tree.
			 */

			if (found_object == NULL) {
				load_object(connection, file.hash, full_path);
				found_object = RB_FIND(Tree_Objects, &Objects, &object);
			}

			/* If the object is still missing, exit. */

			if (found_object == NULL)
				errc(EXIT_FAILURE, ENOENT,
					"process_tree: file %s -- %s cannot be found",
					full_path,
					file.hash);

			/* Otherwise retain it. */

			if ((remote_file = RB_FIND(Tree_Remote_Path, &Remote_Path, &file)) == NULL) {
				new_file_node = (struct file_node *)malloc(sizeof(struct file_node));

				if (new_file_node == NULL)
					err(EXIT_FAILURE,
						"process_tree: malloc");

				new_file_node->mode = file.mode;
				new_file_node->hash = strdup(found_object->hash);
				new_file_node->path = strdup(full_path);
				new_file_node->keep = true;
				new_file_node->save = true;

				RB_INSERT(Tree_Remote_Path, &Remote_Path, new_file_node);
			} else {
				free(remote_file->hash);
				remote_file->mode = file.mode;
				remote_file->hash = strdup(found_object->hash);
				remote_file->keep = true;
				remote_file->save = true;
			}
		}
	}

	/* Add the tree data to the remote data list. */

	release_buffer(connection, tree);
	write(remote_descriptor, buffer, buffer_size);
	write(remote_descriptor, "\n", 1);

	free(buffer);
	free(file.hash);
	free(file.path);
}


/*
 * save_repairs
 *
 * Procedure that saves the repaired files.
 */

static void
save_repairs(connector *connection)
{
	struct object_node  find_object, *found_object;
	struct file_node   *local_file, *remote_file, *found_file;
	char               *check_hash = NULL, *buffer_hash = NULL;
	bool                missing = false, update = false;

	/*
	 * Loop through the remote file list, looking for objects that arrived
	 * in the pack data.
	 */

	RB_FOREACH(found_file, Tree_Remote_Path, &Remote_Path) {
		find_object.hash = found_file->hash;

		found_object = RB_FIND(Tree_Objects, &Objects, &find_object);

		if (found_object == NULL)
			continue;

		/* Save the object. */

		if (S_ISDIR(found_file->mode)) {
			if ((mkdir(found_file->path, 0755) == -1) && (errno != EEXIST))
				err(EXIT_FAILURE,
					"save_repairs: cannot create %s",
					found_file->path);
		} else {
			missing = !path_exists(found_file->path);
			update  = true;

			/*
			 * Because identical files can exist in multiple places,
			 * only update the altered files.
			 */

			if (missing == false) {
				load_buffer(connection, found_object);

				check_hash = calculate_file_hash(
					found_file->path,
					found_file->mode);

				buffer_hash = calculate_object_hash(
					found_object->buffer,
					found_object->buffer_size,
					3);

				release_buffer(connection, found_object);

				if (strncmp(check_hash, buffer_hash, 40) == 0)
					update = false;
			}

			if (update == true) {
				load_buffer(connection, found_object);

				save_file(found_file->path,
					found_file->mode,
					found_object->buffer,
					found_object->buffer_size,
					connection->verbosity,
					connection->display_depth);

				release_buffer(connection, found_object);

				if (strstr(found_file->path, "UPDATING"))
					extend_updating_list(connection,
						found_file->path);
			}
		}
	}

	/* Make sure no files are deleted. */

	RB_FOREACH(remote_file, Tree_Remote_Path, &Remote_Path) {
		local_file = RB_FIND(Tree_Local_Path, &Local_Path, remote_file);

		if (local_file != NULL)
			local_file->keep = true;
	}
}


/*
 * save_objects
 *
 * Procedure that commits the objects and trees to disk.
 */

static void
save_objects(connector *connection)
{
	struct object_node *found_object = NULL, find_object;
	struct file_node   *found_file = NULL;
	char                tree[41], remote_data_file_new[BUFFER_UNIT_SMALL];
	int                 fd;

	/* Find the tree object referenced in the commit. */

	find_object.hash = connection->want;
	found_object     = RB_FIND(Tree_Objects, &Objects, &find_object);

	if (found_object == NULL)
		errc(EXIT_FAILURE, EINVAL,
			"save_objects: cannot find %s",
			connection->want);

	load_buffer(connection, found_object);

	if (memcmp(found_object->buffer, "tree ", 5) != 0)
		errc(EXIT_FAILURE, EINVAL,
			"save_objects: first object is not a commit");

	memcpy(tree, found_object->buffer + 5, 40);
	tree[40] = '\0';

	release_buffer(connection, found_object);

	/* Recursively start processing the tree. */

	snprintf(remote_data_file_new, BUFFER_UNIT_SMALL,
		"%s.new",
		connection->remote_data_file);

	fd = open(remote_data_file_new, O_WRONLY | O_CREAT | O_TRUNC);

	if ((fd == -1) && (errno != EEXIST))
		err(EXIT_FAILURE,
			"save_objects: write file failure %s",
			remote_data_file_new);

	chmod(remote_data_file_new, 0644);
	write(fd, connection->want, strlen(connection->want));
	write(fd, "\n", 1);
	process_tree(connection, fd, tree, connection->path_target);
	close(fd);

	/* Save the remote data list. */

	if (((remove(connection->remote_data_file)) != 0) && (errno != ENOENT))
		err(EXIT_FAILURE,
			"save_objects: cannot remove %s",
			connection->remote_data_file);

	if ((rename(remote_data_file_new, connection->remote_data_file)) != 0)
		err(EXIT_FAILURE,
			"save_objects: cannot rename %s",
			connection->remote_data_file);

	/* Save all of the new and modified files. */

	RB_FOREACH(found_file, Tree_Remote_Path, &Remote_Path) {
		if (!found_file->save)
			continue;

		find_object.hash = found_file->hash;
		found_object = RB_FIND(Tree_Objects, &Objects, &find_object);

		if (found_object == NULL)
			errc(EXIT_FAILURE, EINVAL,
				"save_objects: cannot find %s",
				found_file->hash);

		load_buffer(connection, found_object);

		save_file(found_file->path,
			found_file->mode,
			found_object->buffer,
			found_object->buffer_size,
			connection->verbosity,
			connection->display_depth);

		release_buffer(connection, found_object);

		if (strstr(found_file->path, "UPDATING"))
			extend_updating_list(connection, found_file->path);
	}
}


/*
 * extract_proxy_data
 *
 * Procedure that extracts username/password/host/port data from environment
 * variables.
 */

static void
extract_proxy_data(connector *connection, const char *data)
{
	char *copy = NULL, *temp = NULL, *server = NULL, *port = NULL;
	int   offset = 0;

	if ((data) && (strstr(data, "https://") == data))
		offset = 8;

	if ((data) && (strstr(data, "http://") == data))
		offset = 7;

	if (offset == 0)
		return;

	copy = strdup(data + offset);

	/* Extract the username and password values. */

	if ((temp = strchr(copy, '@')) != NULL) {
		*temp  = '\0';
		server = temp + 1;

		if ((temp = strchr(copy, ':')) != NULL) {
			*temp = '\0';

			free(connection->proxy_username);
			free(connection->proxy_password);

			connection->proxy_username = strdup(copy);
			connection->proxy_password = strdup(temp + 1);
		}
	} else {
		server = copy;
	}

	/* If a trailing slash is present, remove it. */

	if ((temp = strchr(server, '/')) != NULL)
		*temp = '\0';

	/* Extract the host and port values. */

	if (*(server) == '[') {
		server++;

		if ((port = strchr(server, ']')) != NULL)
			*(port++) = '\0';
	} else if ((port = strchr(server, ':')) != NULL)
		*port = '\0';

	if ((server == NULL) || (port == NULL))
		errc(EXIT_FAILURE, EFAULT,
			"extract_proxy_data: malformed host/port %s", data);

	free(connection->proxy_host);
	connection->proxy_host = strdup(server);
	connection->proxy_port = strtol(port + 1, (char **)NULL, 10);

	free(copy);
}


/*
 * load_configuration
 *
 * Procedure that loads the section options from gitup.conf
 */

static int
load_configuration(connector *connection, const char *configuration_file, char **argv, int argc)
{
	struct ucl_parser  *parser = NULL;
	ucl_object_t       *object = NULL;
	const ucl_object_t *section = NULL, *pair = NULL, *ignore = NULL;
	ucl_object_iter_t   it = NULL, it_section = NULL, it_ignores = NULL;
	const char         *key = NULL, *config_section = NULL;
	char               *sections = NULL, temp[BUFFER_UNIT_SMALL];
	unsigned int        sections_size = 0;
	uint8_t             argument_index = 0, x = 0, length = 0;
	struct stat         check_file;

	if ((sections = (char *)malloc(sections_size)) == NULL)
		err(EXIT_FAILURE, "load_configuration: malloc");

	/* Check to make sure the configuration file is actually a file. */

	stat(configuration_file, &check_file);

	if (!S_ISREG(check_file.st_mode))
		errc(EXIT_FAILURE, EFTYPE,
			"load_configuration: cannot load %s",
			configuration_file);

	/* Load and process the configuration file. */

	parser = ucl_parser_new(0);

	if (ucl_parser_add_file(parser, configuration_file) == false) {
		fprintf(stderr,
			"load_configuration: %s\n",
			ucl_parser_get_error(parser));

		exit(EXIT_FAILURE);
	}

	object = ucl_parser_get_object(parser);

	while ((connection->section == NULL) && (section = ucl_iterate_object(object, &it, true))) {
		config_section = ucl_object_key(section);

		/* Look for the section in the command line arguments. */

		for (x = 0; x < argc; x++) {
			if ((strlen(argv[x]) == 2) && (strncmp(argv[x], "-V", 2)) == 0) {
				fprintf(stdout,
					"gitup version %s\n",
					GITUP_VERSION);

				exit(EXIT_SUCCESS);
			}

			if ((strlen(argv[x]) == strlen(config_section)) && (strncmp(argv[x], config_section, strlen(config_section)) == 0)) {
				connection->section = strdup(argv[x]);
				argument_index = x;
			}
		}

		/*
		 * Add the section to the list of known sections in case a
		 * valid section is not found.
		 */

		if (strncmp(config_section, "defaults", 8) != 0) {
			snprintf(temp, sizeof(temp), "\t * %s\n", config_section);
			append(&sections, &sections_size, temp, strlen(temp));

			if (argument_index == 0)
				continue;
		}

		/* Iterate through the section's configuration parameters. */

		while ((pair = ucl_iterate_object(section, &it_section, true))) {
			key = ucl_object_key(pair);

			if (strnstr(key, "branch", 6) != NULL)
				connection->branch = strdup(ucl_object_tostring(pair));

			if (strnstr(key, "display_depth", 16) != NULL) {
				if (ucl_object_type(pair) == UCL_INT)
					connection->display_depth = ucl_object_toint(pair);
				else
					connection->display_depth = strtol(ucl_object_tostring(pair), (char **)NULL, 10);
			}

			if (strnstr(key, "host", 4) != NULL) {
				connection->host = strdup(ucl_object_tostring(pair));

				/* Add brackets to IPv6 addresses, if needed. */

				length = strlen(connection->host) + 3;

				connection->host_bracketed = (char *)realloc(
					connection->host_bracketed,
					length);

				if (connection->host_bracketed == NULL)
					err(EXIT_FAILURE,
						"load_configuration: malloc");

				if ((strchr(connection->host, ':')) && (strchr(connection->host, '[') == NULL))
					snprintf(connection->host_bracketed,
						length,
						"[%s]",
						connection->host);
				else
					snprintf(connection->host_bracketed,
						length,
						"%s",
						connection->host);
			}

			if (((strnstr(key, "ignore", 6) != NULL) || (strnstr(key, "ignores", 7) != NULL)) && (ucl_object_type(pair) == UCL_ARRAY))
				while ((ignore = ucl_iterate_object(pair, &it_ignores, true))) {
					if ((connection->ignore = (char **)realloc(connection->ignore, (connection->ignores + 1) * sizeof(char *))) == NULL)
						err(EXIT_FAILURE, "set_configuration_parameters: malloc");

					snprintf(temp, sizeof(temp), "%s", ucl_object_tostring(ignore));

					if (temp[0] != '/')
						snprintf(temp, sizeof(temp), "%s/%s", connection->path_target, ucl_object_tostring(ignore));

					connection->ignore[connection->ignores++] = strdup(temp);
				}

			if (strnstr(key, "low_memory", 10) != NULL)
				connection->low_memory = ucl_object_toboolean(pair);

			if (strnstr(key, "port", 4) != NULL) {
				if (ucl_object_type(pair) == UCL_INT)
					connection->port = ucl_object_toint(pair);
				else
					connection->port = strtol(ucl_object_tostring(pair), (char **)NULL, 10);
			}

			if (strnstr(key, "proxy_host", 10) != NULL)
				connection->proxy_host = strdup(ucl_object_tostring(pair));

			if (strnstr(key, "proxy_port", 10) != NULL) {
				if (ucl_object_type(pair) == UCL_INT)
					connection->proxy_port = ucl_object_toint(pair);
				else
					connection->proxy_port = strtol(ucl_object_tostring(pair), (char **)NULL, 10);
			}

			if (strnstr(key, "proxy_password", 14) != NULL)
				connection->proxy_password = strdup(ucl_object_tostring(pair));

			if (strnstr(key, "proxy_username", 14) != NULL)
				connection->proxy_username = strdup(ucl_object_tostring(pair));

			if ((strnstr(key, "repository_path", 15) != NULL) || (strnstr(key, "repository", 10) != NULL)) {
				snprintf(temp, sizeof(temp), "%s", ucl_object_tostring(pair));

				if (temp[0] != '/')
					snprintf(temp, sizeof(temp), "/%s", ucl_object_tostring(pair));

				connection->repository_path = strdup(temp);
			}

			if ((strnstr(key, "target_directory", 16) != NULL) || (strnstr(key, "target", 6) != NULL)) {
				connection->path_target = strdup(ucl_object_tostring(pair));

				length = strlen(connection->path_target) - 1;

				if (*(connection->path_target + length) == '/')
					*(connection->path_target + length) = '\0';
			}

			if (strnstr(key, "verbosity", 9) != NULL) {
				if (ucl_object_type(pair) == UCL_INT)
					connection->verbosity = ucl_object_toint(pair);
				else
					connection->verbosity = strtol(ucl_object_tostring(pair), (char **)NULL, 10);
			}

			if (strnstr(key, "work_directory", 14) != NULL)
				connection->path_work = strdup(ucl_object_tostring(pair));
		}
	}

	ucl_object_unref(object);
	ucl_parser_free(parser);

	/*
	 * Check to make sure all of the required information was found in the
	 * configuration file.
	 */

	if (argument_index == 0)
		errc(EXIT_FAILURE, EINVAL,
			"\nCannot find a matching section in the command line "
			"arguments.  These are the configured sections:\n%s",
			sections);

	if (connection->branch == NULL)
		errc(EXIT_FAILURE, EINVAL,
			"No branch found in [%s]",
			connection->section);

	if (connection->host == NULL)
		errc(EXIT_FAILURE, EDESTADDRREQ,
			"No host found in [%s]",
			connection->section);

	if (connection->path_target == NULL)
		errc(EXIT_FAILURE, EINVAL,
			"No target path found in [%s]",
			connection->section);

	if (connection->path_work == NULL)
		errc(EXIT_FAILURE, EINVAL,
			"No work directory found in [%s]",
			connection->section);

	if (connection->port == 0)
		errc(EXIT_FAILURE, EDESTADDRREQ,
			"No port found in [%s]",
			connection->section);

	if (connection->repository_path == NULL)
		errc(EXIT_FAILURE, EINVAL,
			"No repository found in [%s]",
			connection->section);

	/* Extract username/password/host/port from the environment variables. */

	extract_proxy_data(connection, getenv("HTTP_PROXY"));
	extract_proxy_data(connection, getenv("HTTPS_PROXY"));

	free(sections);

	return (argument_index);
}


/*
 * extract_command_line_want
 *
 * Procedure that stores the pack data file from the command line argument,
 * extracts the want and creates the pack commit data file name.
 */

static void
extract_command_line_want(connector *connection, char *option)
{
	char *extension = NULL, *temp = NULL, *want = NULL, *start = NULL;
	int   length = 0;

	if (!path_exists(option))
		err(EXIT_FAILURE,
			"extract_command_line_want: %s",
			option);

	connection->use_pack_file  = true;
	connection->pack_data_file = strdup(option);

	length    = strlen(option);
	start     = option;
	temp      = option;
	extension = strnstr(option, ".pack", length);

	/* Try and extract the want from the file name. */

	while ((temp = strchr(start, '/')) != NULL)
		start = temp + 1;

	want = strnstr(start, connection->section, length - (start - option));

	if (want == NULL)
		return;
	else
		want += strlen(connection->section) + 1;

	if (extension != NULL)
		*extension = '\0';

	if (strlen(want) == 40)
		connection->want = strdup(want);
}


/*
 * usage
 *
 * Procedure that prints a summary of command line options and exits.
 */

static void
usage(const char *configuration_file)
{
	fprintf(stderr,
		"Usage: gitup <section> [-cklrV] [-h checksum] [-t tag] "
		"[-u pack file] [-v verbosity] [-w checksum]\n"
		"  Please see %s for the list of <section> options.\n\n"
		"  Options:\n"
		"    -C  Override the default configuration file.\n"
		"    -c  Force gitup to clone the repository.\n"
		"    -d  Limit the display of changes to the specified number of\n"
		"          directory levels deep (0 = display the entire path).\n"
		"    -h  Override the 'have' checksum.\n"
		"    -k  Save a copy of the pack data to the current working directory.\n"
		"    -l  Low memory mode -- stores temporary object data to disk.\n"
		"    -r  Repair all missing/modified files in the local repository.\n"
		"    -t  Fetch the commit referenced by the specified tag.\n"
		"    -u  Path to load a copy of the pack data, skipping the download.\n"
		"    -v  How verbose the output should be (0 = no output, 1 = the default\n"
		"          normal output, 2 = also show debugging information).\n"
		"    -V  Display gitup's version number and exit.\n"
		"    -w  Override the 'want' checksum.\n"
		"\n", configuration_file);

	exit(EXIT_FAILURE);
}


/*
 * main
 *
 * A lightweight, dependency-free program to clone/pull a Git repository.
 */

int
main(int argc, char **argv)
{
	struct object_node *object = NULL, *next_object = NULL;
	struct file_node   *file   = NULL, *next_file   = NULL;
	const char         *configuration_file = CONFIG_FILE_PATH;

	char     *command = NULL, *display_path = NULL, *temp = NULL;
	char      base64_credentials[BUFFER_UNIT_SMALL];
	char      credentials[BUFFER_UNIT_SMALL];
	char      section[BUFFER_UNIT_SMALL];
	char      gitup_revision[BUFFER_UNIT_SMALL];
	char      gitup_revision_path[BUFFER_UNIT_SMALL];
	int       x = 0, option = 0, length = 0;
	int       base64_credentials_length = 0, skip_optind = 0;
	uint32_t  o = 0;
	bool      encoded = false, just_added = false;
	bool      current_repository = false, path_target_exists = false;
	bool      remote_data_exists = false, pack_data_exists = false;

#if OPENSSL_VERSION_NUMBER < 0x10100000L
	EVP_ENCODE_CTX      evp_ctx;
#else
	EVP_ENCODE_CTX     *evp_ctx;
#endif

	connector connection = {
		.ssl               = NULL,
		.ctx               = NULL,
		.socket_descriptor = 0,
		.host              = NULL,
		.host_bracketed    = NULL,
		.port              = 0,
		.proxy_host        = NULL,
		.proxy_port        = 0,
		.proxy_username    = NULL,
		.proxy_password    = NULL,
		.proxy_credentials = NULL,
		.section           = NULL,
		.repository_path   = NULL,
		.branch            = NULL,
		.tag               = NULL,
		.have              = NULL,
		.want              = NULL,
		.response          = NULL,
		.response_blocks   = 0,
		.response_size     = 0,
		.clone             = false,
		.repair            = false,
		.object            = NULL,
		.objects           = 0,
		.pack_data_file    = NULL,
		.path_target       = NULL,
		.path_work         = NULL,
		.remote_data_file  = NULL,
		.ignore            = NULL,
		.ignores           = 0,
		.keep_pack_file    = false,
		.use_pack_file     = false,
		.verbosity         = 1,
		.display_depth     = 0,
		.updating          = NULL,
		.back_store        = -1,
		.low_memory        = false,
		};

	if (argc < 2)
		usage(configuration_file);

	/* Check to see if the configuration file path is being overridden. */

	for (x = 0; x < argc; x++)
		if ((strlen(argv[x]) > 1) && (strnstr(argv[x], "-C", 2) == argv[x])) {
			if (strlen(argv[x]) > 2)
				configuration_file = strdup(argv[x] + 2);
			else if ((argc > x) && (argv[x + 1][0] != '-'))
				configuration_file = strdup(argv[x + 1]);
		}

	/* Load the configuration file to learn what section is being requested. */

	skip_optind = load_configuration(&connection, configuration_file, argv, argc);

	if (skip_optind == 1)
		optind++;

	/* Process the command line parameters. */

	while ((option = getopt(argc, argv, "C:cd:h:klrt:u:v:w:")) != -1) {
		switch (option) {
			case 'C':
				if (connection.verbosity)
					fprintf(stderr,
						"# Configuration file: %s\n",
						configuration_file);
				break;
			case 'c':
				connection.clone = true;
				break;
			case 'd':
				connection.display_depth = strtol(optarg, (char **)NULL, 10);
				break;
			case 'h':
				connection.have = strdup(optarg);
				break;
			case 'k':
				connection.keep_pack_file = true;
				break;
			case 'l':
				connection.low_memory = true;
				break;
			case 'r':
				connection.repair = true;
				break;
			case 't':
				connection.tag = strdup(optarg);
				break;
			case 'u':
				extract_command_line_want(&connection, optarg);
				break;
			case 'v':
				connection.verbosity = strtol(optarg, (char **)NULL, 10);
				break;
			case 'w':
				connection.want = strdup(optarg);
				break;
		}

		if (optind == skip_optind)
			optind++;
	}

	/* Build the proxy credentials string. */

	if (connection.proxy_username) {
		snprintf(credentials, sizeof(credentials),
			"%s:%s",
			connection.proxy_username,
			connection.proxy_password);

#if OPENSSL_VERSION_NUMBER < 0x10100000L
		EVP_EncodeInit(&evp_ctx);

		EVP_EncodeUpdate(&evp_ctx,
			(unsigned char *)base64_credentials,
			&base64_credentials_length,
			(unsigned char *)credentials,
			strlen(credentials));

		EVP_EncodeFinal(&evp_ctx,
			(unsigned char *)base64_credentials,
			&base64_credentials_length);
#else
		evp_ctx = EVP_ENCODE_CTX_new();

		EVP_EncodeInit(evp_ctx);

		EVP_EncodeUpdate(evp_ctx,
			(unsigned char *)base64_credentials,
			&base64_credentials_length,
			(unsigned char *)credentials,
			strlen(credentials));

		EVP_EncodeFinal(evp_ctx,
			(unsigned char *)base64_credentials,
			&base64_credentials_length);

		EVP_ENCODE_CTX_free(evp_ctx);
#endif

		/* Remove the trailing return. */

		if ((temp = strchr(base64_credentials, '\n')) != NULL)
			*temp = '\0';

		length = 30 + strlen(base64_credentials);

		connection.proxy_credentials = (char *)malloc(length + 1);

		if (connection.proxy_credentials == NULL)
			err(EXIT_FAILURE, "main: malloc");

		snprintf(connection.proxy_credentials, length,
			"Proxy-Authorization: Basic %s\r\n",
			base64_credentials);
	} else {
		connection.proxy_credentials = (char *)malloc(1);

		if (connection.proxy_credentials == NULL)
			err(EXIT_FAILURE, "main: malloc");

		connection.proxy_credentials[0] = '\0';
	}

	/* If a tag and a want are specified, warn and exit. */

	if ((connection.tag != NULL) && (connection.want != NULL))
		errc(EXIT_FAILURE, EINVAL,
			"A tag and a want cannot both be requested");

	/* Create the work path and build the remote data path. */

	make_path(connection.path_work, 0755);

	length = strlen(connection.path_work) + strlen(connection.section) + 1;

	connection.remote_data_file = (char *)malloc(length + 1);

	if (connection.remote_data_file == NULL)
		err(EXIT_FAILURE, "main: malloc");

	snprintf(connection.remote_data_file, length + 1,
		"%s/%s",
		connection.path_work,
		connection.section);

	temp = strdup(connection.remote_data_file);

	/* If non-alphanumerics exist in the section, encode them. */

	length = strlen(connection.section);

	for (x = 0; x < length - 1; x++)
		if ((!isalpha(connection.section[x])) && (!isdigit(connection.section[x]))) {
			if ((connection.section = (char *)realloc(connection.section, length + 2)) == NULL)
				err(EXIT_FAILURE, "main: realloc");

			memcpy(section, connection.section + x + 1, length - x);
			snprintf(connection.section + x, length - x + 3,
				"%%%X%s",
				connection.section[x],
				section);

			length += 2;
			x += 2;
			encoded = true;
		}

	if (encoded == true) {
		/* Store the updated remote data path. */

		length += strlen(connection.path_work) + 1;

		if ((connection.remote_data_file = (char *)realloc(connection.remote_data_file, length + 1)) == NULL)
			err(EXIT_FAILURE, "main: realloc");

		snprintf(connection.remote_data_file, length + 1,
			"%s/%s",
			connection.path_work,
			connection.section);

		/* If a non-encoded remote data path exists, try and rename it. */

		if ((path_exists(temp)) && ((rename(temp, connection.remote_data_file)) != 0))
			err(EXIT_FAILURE,
				"main: cannot rename %s",
				connection.remote_data_file);
	}

	free(temp);

	/*
	 * If the remote files list or repository are missing, then a clone
	 * must be performed.
	 */

	path_target_exists  = path_exists(connection.path_target);
	remote_data_exists  = path_exists(connection.remote_data_file);
	pack_data_exists    = path_exists(connection.pack_data_file);

	if ((path_target_exists == true) && (remote_data_exists == true))
		load_remote_data(&connection);
	else
		connection.clone = true;

	if (path_target_exists == true) {
		if (connection.verbosity)
			fprintf(stderr, "# Scanning local repository...");

		scan_local_repository(&connection, connection.path_target);

		if (connection.verbosity)
			fprintf(stderr, "\n");
	} else {
		connection.clone = true;
	}

	/* Display connection parameters. */

	if (connection.verbosity) {
		fprintf(stderr, "# Host: %s\n", connection.host);
		fprintf(stderr, "# Port: %d\n", connection.port);

		if (connection.proxy_host)
			fprintf(stderr,
				"# Proxy Host: %s\n"
				"# Proxy Port: %d\n",
				connection.proxy_host,
				connection.proxy_port);

		if (connection.proxy_username)
			fprintf(stderr,
				"# Proxy Username: %s\n",
				connection.proxy_username);

		fprintf(stderr,
			"# Repository Path: %s\n"
			"# Target Directory: %s\n",
			connection.repository_path,
			connection.path_target);

		if (connection.use_pack_file == true)
			fprintf(stderr,
				"# Using pack file: %s\n",
				connection.pack_data_file);

		if (connection.tag)
			fprintf(stderr, "# Tag: %s\n", connection.tag);

		if (connection.have)
			fprintf(stderr, "# Have: %s\n", connection.have);

		if (connection.want)
			fprintf(stderr, "# Want: %s\n", connection.want);

		if (connection.low_memory)
			fprintf(stderr, "# Low memory mode: Yes\n");
	}

	/* Adjust the display depth to include path_target. */

	if (connection.display_depth > 0) {
		temp = connection.path_target;

		while ((temp = strchr(temp + 1, '/')))
			connection.display_depth++;
	}

	/* Setup the connection to the server. */

	connect_server(&connection);

	if (connection.proxy_host)
		create_tunnel(&connection);

	setup_ssl(&connection);

	/* Execute the fetch, unpack, apply deltas and save. */

	if ((connection.use_pack_file == true) && (pack_data_exists == true)) {
		if (connection.verbosity)
			fprintf(stderr,
				"# Action: %s\n",
				(connection.clone ? "clone" : "pull"));

		load_pack(&connection);
		apply_deltas(&connection);
		save_objects(&connection);
	} else {
		if ((connection.use_pack_file == false) || ((connection.use_pack_file == true) && (pack_data_exists == false)))
			get_commit_details(&connection);

		if ((connection.have != NULL) && (connection.want != NULL) && (strncmp(connection.have, connection.want, 40) == 0))
			current_repository = true;

		/* When pulling, first ensure the local tree is pristine. */

		if ((connection.repair == true) || (connection.clone == false)) {
			command = build_repair_command(&connection);

			if (command != NULL) {
				connection.repair = true;

				if (connection.verbosity)
					fprintf(stderr, "# Action: repair\n");

				fetch_pack(&connection, command);
				apply_deltas(&connection);
				save_repairs(&connection);
			}
		}

		/* Process the clone or pull. */

		if ((current_repository == false) && (connection.repair == false)) {
			if (connection.verbosity)
				fprintf(stderr,
					"# Action: %s\n",
					(connection.clone ? "clone" : "pull"));

			if (connection.clone == false)
				command = build_pull_command(&connection);
			else
				command = build_clone_command(&connection);

			fetch_pack(&connection, command);
			apply_deltas(&connection);
			save_objects(&connection);
		}
	}

	/* Save .gituprevision. */

	if ((connection.want) || (connection.tag)) {
		snprintf(gitup_revision_path, BUFFER_UNIT_SMALL,
			"%s/.gituprevision",
			connection.path_target);

		snprintf(gitup_revision, BUFFER_UNIT_SMALL,
			"%s:%.9s\n",
			(connection.tag ? connection.tag : connection.branch),
			connection.want);

		save_file(gitup_revision_path,
			0644,
			gitup_revision,
			strlen(gitup_revision),
			0,
			0);
	}

	/* Wrap it all up. */

	RB_FOREACH_SAFE(file, Tree_Local_Hash, &Local_Hash, next_file)
		RB_REMOVE(Tree_Local_Hash, &Local_Hash, file);

	RB_FOREACH_SAFE(file, Tree_Local_Path, &Local_Path, next_file) {
		if ((file->keep == false) && ((current_repository == false) || (connection.repair == true))) {
			if (ignore_file(&connection, file->path))
				continue;

			if ((connection.verbosity) && (connection.display_depth == 0))
				printf(" - %s\n", file->path);

			if (S_ISDIR(file->mode)) {
				display_path = trim_path(file->path,
					connection.display_depth,
					&just_added);

				if ((connection.verbosity) && (connection.display_depth > 0) && (just_added) && (strlen(display_path) == strlen(file->path)))
					printf(" - %s\n", display_path);

				prune_tree(&connection, file->path);
				free(display_path);
			} else if ((remove(file->path)) && (errno != ENOENT)) {
				fprintf(stderr,
					" ! cannot remove %s\n",
					file->path);
			}
		}

		RB_REMOVE(Tree_Local_Path, &Local_Path, file);
		file_node_free(file);
	}

	RB_FOREACH_SAFE(file, Tree_Remote_Path, &Remote_Path, next_file) {
		RB_REMOVE(Tree_Remote_Path, &Remote_Path, file);
		file_node_free(file);
	}

	RB_FOREACH_SAFE(file, Tree_Trim_Path, &Trim_Path, next_file) {
		RB_REMOVE(Tree_Trim_Path, &Trim_Path, file);
		file_node_free(file);
	}

	RB_FOREACH_SAFE(object, Tree_Objects, &Objects, next_object)
		RB_REMOVE(Tree_Objects, &Objects, object);

	for (o = 0; o < connection.objects; o++) {
		if (connection.verbosity > 1)
			fprintf(stdout,
				"###### %05d-%d\t%d\t%u\t%s\t%d\t%s\n",
				connection.object[o]->index,
				connection.object[o]->type,
				connection.object[o]->pack_offset,
				connection.object[o]->buffer_size,
				connection.object[o]->hash,
				connection.object[o]->index_delta,
				connection.object[o]->ref_delta_hash);

		object_node_free(connection.object[o]);
	}

	if ((connection.verbosity) && (connection.updating))
		fprintf(stderr,
			"#\n# Please review the following file(s) for "
			"important changes.\n%s#\n",
			connection.updating);

	for (x = 0; x < connection.ignores; x++)
		free(connection.ignore[x]);

	free(connection.ignore);
	free(connection.response);
	free(connection.object);
	free(connection.host);
	free(connection.host_bracketed);
	free(connection.proxy_host);
	free(connection.proxy_username);
	free(connection.proxy_password);
	free(connection.proxy_credentials);
	free(connection.section);
	free(connection.repository_path);
	free(connection.branch);
	free(connection.tag);
	free(connection.have);
	free(connection.want);
	free(connection.pack_data_file);
	free(connection.path_target);
	free(connection.path_work);
	free(connection.remote_data_file);
	free(connection.updating);

	if (connection.ssl) {
		SSL_shutdown(connection.ssl);
		SSL_CTX_free(connection.ctx);
		close(connection.socket_descriptor);
		SSL_free(connection.ssl);
	}

	if (connection.repair == true)
		fprintf(stderr,
			"# The local repository has been repaired.  "
			"Please rerun gitup to pull the latest commit.\n");

	if (connection.verbosity)
		fprintf(stderr, "# Done.\n");

	sync();

	return (0);
}

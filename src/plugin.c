/* plugin.c
 * Copyright (C) 2006 Tillmann Werner <tillmann.werner@gmx.de>
 *
 * This file is free software; as a special exception the author gives
 * unlimited permission to copy and/or distribute it, with or without
 * modifications, as long as this notice is preserved.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY, to the extent permitted by law; without even the
 * implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 */

#include <stdio.h>
#include <dlfcn.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fnmatch.h>
#include <stdlib.h>

#include "honeytrap.h"
#include "plugin.h"
#include "plughook.h"
#include "logging.h"


int load_plugins(char *dir) {
	struct stat statbuf;
	struct dirent **namelist;
	int n;
	char *full_path;
	DIR *plugindir;

	full_path = NULL;
	plugin_list = NULL;

	init_plugin_hooks();

	if ((plugindir = opendir(dir)) == NULL) {
		closedir(plugindir);
		fprintf(stderr, "  Error - Plugin directory is not readable.\n");
		return(-1);
	}
	
	DEBUG_FPRINTF(stdout, "  Searching for plugins in %s\n", dir);
	if ((n = scandir(dir, &namelist, 0, alphasort)) < 0) {
		fprintf(stderr, "  Error - Unable to scan plugin directory: %s\n", strerror(errno));
		return(-1);
	} else while(n--) {
		stat(namelist[n]->d_name, &statbuf);
		if (fnmatch("htm_*.so", namelist[n]->d_name, 0) == 0) {
			/* found a plugin */
			if ((full_path = (char *) malloc(strlen(dir) + strlen(namelist[n]->d_name) + 2)) == NULL) {
				fprintf(stderr, "  Error - Unable to allocate memory: %s\n", strerror(errno));
				return(-1);
			}
			snprintf(full_path, strlen(dir)+strlen(namelist[n]->d_name)+2, "%s/%s", dir, namelist[n]->d_name);
			DEBUG_FPRINTF(stdout, "  Plugin found: %s\n", full_path);
			init_plugin(full_path);
			free(full_path);
		}
		free(namelist[n]);
	}
	free(namelist);
	
	return(0);
}

int init_plugin(char *plugin_name) {
	int (*init_plugin)();
	void (*unload_plugin)();
	int retval;
	Plugin *last_plugin, *new_plugin;

	/* allocate memory for new plugin and attach it to the plugin list */
	if ((new_plugin = (Plugin *) malloc(sizeof(Plugin))) == NULL) {
		fprintf(stderr, "    Error - Unable to allocate memory: %s\n", strerror(errno));
		return(-1);
	} else {
		new_plugin->handle = NULL;
		new_plugin->name = NULL;
		new_plugin->version = NULL;
		new_plugin->next = NULL;
		new_plugin->filename = NULL;
	}

	if (plugin_name == NULL) {
		fprintf(stderr, "  Error loading plugin - No name given.\n");
		return(-1);
	} else { 
		if ((new_plugin->handle = (void *) malloc(sizeof(int))) == NULL) { 
			fprintf(stderr, "  Error loading plugin - Unable to allocate memory: %s\n", strerror(errno));
			return(-1);
		} else new_plugin->filename = (char *) strdup(plugin_name);
	}
	DEBUG_FPRINTF(stdout, "  Loading plugin %s.\n", new_plugin->filename);

	dlerror();      /* Clear any existing error */
	if (((new_plugin->handle = dlopen(new_plugin->filename, RTLD_NOW)) == NULL) &&
	    ((plugin_error_str = (char *) dlerror()) != NULL)) {
		fprintf(stderr, "  Unable to initialize plugin: %s\n", plugin_error_str);
		unload_at_err(new_plugin);
		return(-1);
	}

	/* determin internal module name and version string */
	if (((new_plugin->name = (char *) dlsym(new_plugin->handle, "module_name")) == NULL) &&
	    ((plugin_error_str = (char *) dlerror()) != NULL)) {
		/* handle error, the symbol wasn't found */
		fprintf(stderr, "  Unable to initialize plugin: %s\n", plugin_error_str);
		fprintf(stderr, "  %s seems not to be a honeytrap plugin.\n", new_plugin->filename);
		unload_at_err(new_plugin);
		return(-1);
	}
	if (((new_plugin->version = (char *) dlsym(new_plugin->handle, "module_version")) == NULL) &&
	    ((plugin_error_str = (char *) dlerror()) != NULL)) {
		/* handle error, the symbol wasn't found */
		fprintf(stderr, "  Unable to initialize plugin %s: %s\n", new_plugin->name, plugin_error_str);
		fprintf(stderr, "  %s seems not to be a honeytrap plugin.\n", new_plugin->filename);
		unload_at_err(new_plugin);
		return(-1);
	}
	DEBUG_FPRINTF(stdout, "  Loaded plugin %s v%s.\n", new_plugin->name, new_plugin->version);

	DEBUG_FPRINTF(stdout, "  Initializing plugin %s.\n", new_plugin->name);
	/* resolve module's unload function and add it to unload hook */
	if (((unload_plugin = dlsym(new_plugin->handle, "plugin_unload")) == NULL) && 
	    ((plugin_error_str = (char *) dlerror()) != NULL)) {
		/* handle error, the symbol wasn't found */
		fprintf(stderr, "    Unable to initialize plugin %s: %s\n", new_plugin->name, plugin_error_str);
		fprintf(stderr, "    %s seems not to be a honeytrap plugin.\n", new_plugin->filename);
		unload_at_err(new_plugin);
		return(-1);
	}
	if (!add_unload_func_to_list(new_plugin->name, "plugin_unload", unload_plugin)) {
		fprintf(stderr, "    Unable to register module for hook 'unload_plugins': %s\n", plugin_error_str);
		unload_at_err(new_plugin);
		return(-1);
	}


	/* resolve and call module's init function */
	if (((init_plugin = dlsym(new_plugin->handle, "plugin_init")) == NULL) && 
	    ((plugin_error_str = (char *) dlerror()) != NULL)) {
		/* handle error, the symbol wasn't found */
		fprintf(stderr, "    Unable to resolve symbol 'plugin_init': %s\n", plugin_error_str);
		return(-1);
	}
	retval = -1;
	init_plugin();
	fprintf(stdout, "  Initialized plugin %s.\n", new_plugin->name);

	/* attach plugin to plugin_list */
	if (!plugin_list) plugin_list = new_plugin;
	else {
		last_plugin = plugin_list;
		while(last_plugin->next) last_plugin = last_plugin->next;
		last_plugin->next = new_plugin;
	}
	
	return(1);
}


void unload_plugins(void) {
	Plugin *cur_plugin;
	
	/* call unload functions from plugins */
	plughook_unload_plugins();
	
	/* unload plugin and free mem for filename and plugin 
	 * other variables in struct are symbols and do not need to be freed */
	while(plugin_list) {
		cur_plugin = plugin_list->next;
		if (dlclose(plugin_list->handle) != 0)
			logmsg(LOG_ERR, 1, "Error - Unable to unload plugin %s.\n", plugin_list->name);
		free(plugin_list->filename);
		free(plugin_list);
		plugin_list = cur_plugin;
	}
	return;
}


void unload_at_err(Plugin *plugin) {
	fprintf(stderr, "  Unloading plugin on initialization error.\n");
	if (plugin) {
		if (plugin->handle) dlclose(plugin->handle);
		free(plugin->filename);
	}
	free(plugin);
	return;
}
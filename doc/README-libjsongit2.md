libjsongit2
-----------

Gitohashi contains a lightweight C library using libgit2 that converts
urlpaths into JSON representing the "business logic" of a sophisticated gitweb
type interface.  Gitohashi uses its library and provides assets to present its
JSON representation in HTML, but libjsongit2's JSON representation could also
be represented completely different by different applications using it.

![libjsongit2-overview](./doc/doc-assets/libjsongit2-overview.svg)

Try it out via gitohashi at https://libwebsockets.org/git and
https://warmcat.com/git

## Features

 - library implements a "gitweb" type interface to bare git repos, taking a
   cgit-compatible "urlpath" and returning JSON, an HTML template plus JSON,
   or a direct file from inside a repo revision with its own mimetype,
   with the presentation entirely separated and done clientside.

 - Fast, small, modern, stateful, multivhost, threadsafe, opaque C api with
   just 5 functions, no libgit2 types.

 - Works with libgit2 v0.19+ ie, Ubuntu 14.04+; blame support requires
   libgit2 0.21+

 - Per-connection stateful context-based api bound to a vhost allows unlimited
   number of concurrent ongoing requests on same or different repos

 - Output generated JIT to fill user buffer, state maintain to generate more
   only when the user code wants to fill another buffer.

 - Transparent caching at JSON block level, keyed using global repository ref
   state... cache invalidated when any ref updated.  No time-based caching.

 - Top level vhost context allows small number of settings like vurl, and
   caching things per-vhost.  If using HTML sandwiching, the vhost loads the
   html template into memory one time.

 - Uses existing repo configuration where possible:

    - gitweb config options in the repo itself

    - gitolite ACL config parsed if found in the vhost's repository directory
      selects which repos are visible based on vhost's "user" and per-context
      authenticated user

 - Very modest on memory, Valgrind-clean, Coverity-clean

 - Highly optimized Gravatar / md5 support

 - Supports serving http assets like pictures directly from the repo, so
   they are versioned along with the ref being viewed

 - Dynamic, stateful snapshot archive generation for tar.gz, tar.bz2, tar.xz
   and zip
   
 - Enables Internationalization, passes the browser `ACCEPT_LANGUAGE` header
   data back in the JSON.  But since internationalization is about presentation,
   it is actually performed clientside.
   
 - various alignments to github style, such as:
 
    - inline README.md display in tree view
    
    - markdown can fetch from the repo with URLs starting ./ for the
   current directory the markdown is at inside the repo
   
    - line range highlighting
    
    - sorted contributor list in blame view

 - CMake crossplatform, simple, adaptive build system

## Getting Started

libjsongit2 is a cmake project that's easy to build.  It requires libgit2 and
optionally libarchive, both of which are available in all popular distros.

Full build details: [README-build.md](./doc/README-build.md)

## Overview

This C library gives you a way to get Read-Only access to selected repos
according to your gitolite ACLs, in HTML + JSON and also to directly access repo
files for a particular rev in their native mimetype.  From a URL path, it
returns serveable HTTP reflecting various ways of looking at the repo and its
contents as needed by a gitweb type application.

![naked-and-sandwich](./doc/doc-assets/naked-and-sandwich.svg)

After initializing at least vhost with this library, you create per-connection
"contexts" which are associated with a repo in the filesystem and a "urlpath"
describing the information you want in JSON.

Based on the urlpath, the library emits one or more "jobs", either HTML + JSON
or raw files served from a repo rev.  Each time there's no more space left in
the user output buffer for another entry, the library returns and waits to be
called again to fill another buffer.  So the library does not create content
until it's directly possible to send it on, and the caller is explicitly in
control of how much data is produced and when.  Many different connection
contexts may be ongoing simultaneously.

The library uses the urlpath to provide all the kinds of information needed
for gitweb style rendering at the client.

Jobs can be chained together so different kinds of information can coexist in a
single JSON blob.

The context holds all the necessary state, the user code chooses when to ask a
context to continue to generate another buffer of JSON.  So it's able to only do
work (and use memory) to generate data that can be sent onwards immediately,
eliminating stream buffering at the server side.

### HTML template "sandwich"

To simplify customization and use, libjsongit2 can produce JSON already
embedded in a template HTML.  An HTML comment marks where the JSON should be
inserted.

![libjsongit2-sandwich](./doc/doc-assets/libjsongit2-sandwich.svg)

The template HTML also makes it easy to directly configure css and related js
needed both by the template and libjsongit2 parts in the template itself.

The HTML template is cached in memory by the libjsongit2 vhost, but it checks
each time it's used if it has been longer than 5s since the last check, that
it's up to date with the original file in the filesystem, and auto-reloads if
not.

### JSON output

The library produces well-formed JSON ready for `JSON.parse()` that
always has an outer container like this

```
{
 "schema":"jg2-1",
 ... dynamic entries ...
 "items": [

... one or more "job" JSON { structures }, may be from cache ...

 ],
 ... dynamic stats entries ...
}
```

cacheable "items" that are relevant to the requested urlpath are provided.

Full details and documentation on the JSON format: [README-JSON.md](./doc/README-JSON.md)

The jg2-example application built along with libjsongit2 allows you to see the
JSON produced for a given URLpath.

### URL structure

The generated JSON embeds links and interprets url paths (the part after
`http[s]://xxx.com`) using the following rules, broadly compatible with cgit:

**/vpath**/reponame/**mode**/repopath\[**?h=branch**\]

 - vpath: the server can set `.virtual_base_urlpath` when creating the
   "vhost" using the `struct jg2_vhost_config *` passed as
   an argument to `jg2_vhost_create()`

 - reponame: which repo

 - mode: one of:
    - (the url ends before this): see summary
    - "refs": exhaustive list of refs in the repo
    - "log": history from a specific ref or commit
    - "tree": view of the file structure behind the commit chain
    - "commit": the actual diff view of a single commit
    - "plain": a blob with a guessed mimetype
    - "patch": plain text raw patch (text/plain mimetype)
    - "snapshot": various kinds of archive of a specific ref or commit
    - "blame": like tree but with extra provonance information
    - "branches": exhaustive list of branches
    - "tags": exhaustive list of tags
    - "summary": rundown of the top ten most-recently updated branches and tags

 - repopath: the path inside the repo

 - URLargs that restrict the context of the request include:
    - `?h=branch`: specifies a branch (default is "master")
    - `?id=<oid hex representation>`
    - `?ofs=<number of items>`


### Gravatar support

The library maintains a hashtable of most recently seen email md5s in the opaque
"vhost" structure.  By default, this is 16 hash bins each a max of 16 deep, or
256 md5s, or about 20KiB including space for 64-char emails.  These numbers can
be overridden at `jg2_vhost_create()` time.

Because it's in the `jg2_vhost` struct, which contexts bind to at creation time,
the email cache is shared between all contexts using the same vhost.

The email cache algorithm moves email matches to the start of the bin's linked-
list each time, and after the hash bin is full, it recycles the least-recently
seen email at the end of the list.

It's also possible to override the internal md5 code with an external function
that may be faster, in the user config at the vhost init / vhost creation time.
See `struct jg2_vhost_config` in `libjsongit2.h`

[Gitohashi](https://warmcat.com/git/gitohashi) additionally provides an avatar proxy.

## API overview

The user api is defined in `./include/libjsongit2.h`, it consists of just 5
functions.

To init:

 - `jg2_vhost_create()` once per vhost, but at least once (eg, on app init).
   An args struct is passed which allows configuring vhost options in an
   extensible way.  If the app starts as root and drops privileges, the vhosts
   should be created while still root, allowing low tcp port binding and auto
   creation and persmissions setting on the JSON cache dir.

 - `jg2_ctx_create()` once per connection.  An args struct is passed which
   allows configuring the connection options in an extensible way.  When the
   context is created, the URL related to it is parsed to understand what kind
   of result is needed, along with returning the mimetype and size if possible
   to the caller.

To emit the output (JSON, HTML + JSON, raw file contents etc):

 - `jg2_ctx_fill()` to write the next chunk of output into a provided buffer.
   As far as possible output is only generated a buffer at a time.

To finish up:

 - `jg2_ctx_destroy()` for every created context (eg, on connection close)

 - `jg2_vhost_destroy()` for every vhost init (eg, on app close)

## Configuration

libjsongit2 tries to eliminate as much configuration as possible.  It does this
by re-using existing git-related conventions like gitweb config and gitolite
ACLs, instead of repeating separate lists of repos to be shown.

### Per-vhost configuration

When creating the vhost object, there's a public `struct jg2_vhost_config`
config struct that can be filled in, but only two items are mandatory.

 - `virtual_base_urlpath`: the virtual URL part, eg, /git

 - `repo_base_dir`: the base directories where the git repos live
 
 See https://warmcat.com/git/libjsongit2/tree/include/libjsongit2.h for the
 full set of vhost configuration arguments.

Full-details of gitolite integration: [README-gitolite.md](./doc/README-gitolite.md)  

### Example app

A minimal example commandline app is built with the library, if you point
it to a dir where bare repos live, and give it a "url path", it will dump
JSON to stdout.

See ./examples/minimal/jg2-example.c

## Transparent JSON Cache

libjsongit2 can cache the results of individual JSON jobs and "naked"
generated files like snapshots and content served direct from a repo ref.

The user can set the cache location and size limit per repository base dir,
and a thread scans the cache lazily deleting files on an LRU basis once the
cache reaches its limit.  The scan uses a few KiB of memory regardless of the
size of the cache.

The cached files use many keys to ensure they are relevant to the requested
URL.  Full details: [README-cache.md](./doc/README-cache.md) 

## Contact

Andy Green &lt;andy@warmcat.com&gt;


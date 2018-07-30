## JSON output

The library produces well-formed JSON ready for `JSON.parse()` that
always has an outer container like this

```
{
 "schema":"jg2-1",
 ... dynamic entries ...
 "items": [

... one or more "job" JSON `{ structures }`, may be from cache ...

 ],
 ... dynamic stats entries ...
}
```

### Dynamic JSON header entries

JSON name|Meaning
---|---
schema|always "jg2-1" currently
vpath|The virtual URL path the server is at, eg, "/git/".  URL paths back to the server must start with this
avatar|URL path to link to the avatar cache, eg, "/avatar/"
alang|Whatever the browser gave to us for `ACCEPT_LANGUAGE`
f|server flags... b0 = 1: server can handle blame, b1 = 1 = server can do snapshot archives, b2 = 1 = blog mode
gen_ut|Unix time this page was created
reponame|Name of the repository this refers to
desc|Description of the project in the repository from gitweb
owner|`{identity}` structure (see later) using data from gitweb
url|git clone url from gitweb

Note on `"f"`... libjsongit2 builds adaptively to the featureset in the version
of libgit2 that it's linked to and whether libarchive was present.  b0 and b1
let us know if the server can handle blame (libgit > 0.21) and creating
snapshot archives (libarchive required), so the UI can adapt accordingly.

b2, "blog mode" parses markdown in the git repo as a semi-static blog; if set
it implies a simplified UI should be shown, eg, without the mode tabs or tree
directory view.  

### Dynamic JSON trailer entries

JSON name|Meaning
---|---
g|us (microseconds) taken to produce the overall JSON
chitpc|Percentage of JSON cache hits for this vhost
ehitpc|Percentage of ETAG cache hits for this vhost

### Identity structure

JSON name|Meaning
---|---
name|Name like "Fred Bloggs"
email|Email like fred@bloggs.com
md5|md5sum of the email (used with gravatar)

### OID structure

JSON name|Meaning
---|---
oid|OID the information applies to
alias|List of refs that share this OID

### git_time structure

JSON name|Meaning
---|---
time|Unix time
offset|Timezone offset from unix time in minutes

### Signature structure

JSON name|Meaning
---|---
git_time|`{git_time structure}`
name|Name
email|email
md5|md5sum of the email

### Job JSON structure

A single URLpath "connection context" may need to spawn multiple "jobs" in a
single response.  For example, if there is a README.md in the directory, the
`"items": []` array will contain two JSON items, one from a "/tree/" job to show
the directory contents, and another showing the contents of the README.md blob
for the revision being shown.

The contents of the Job JSON depends on the job type... and the kind of jobs
that can appear are set by the URLpath the JSON is returned for.   Eg the JSON
for a tree directory listing is (for a `/tree/` URL) is like this:

```
{ "schema":"libjg2-1",
 "oid":{ "oid": "79397b21a38d263263a65abb69b8e1fffd326796",
 "alias": [ "refs/heads/master"]},"tree": [ 
{ "name": "READMEs","mode": "16384", "size":0},
{ "name": "cmake","mode": "16384", "size":0},
{ "name": "contrib","mode": "16384", "size":0},
...
{ "name": "component.mk","mode": "33188", "size":1659},
{ "name": "libwebsockets.dox","mode": "33188", "size":11618}],
"s":{"c":1534293661,"u":2442}}
```

### Job JSON header

Ever job JSON starts with the same header, which also goes into the cached
version.

JSON name|Meaning
---|---
schema|"libjg2-1"
cid|If cache enabled, and relevant to the job, the cache hash of this JSON
oid|`{OID structure}` described above; present if content is related to an oid

### Job JSON trailer

Every "job" JSON has this information appended and the information is also
part of the cached copy.

JSON name|Meaning
---|---
c|Creation Unix Time
u|us (microseconds) taken to create originally

## Job-specific JSON reference

### tree directory

Outer JSON name: **tree**

Comprises an array of structures of the form

JSON name|Meaning
---|---
name|The file name in the directory
mode|low 9 bits are xrw bits for owner, group, other, b14 means directory
size|size of the blob in bytes

### tree file

JSON name|Meaning
---|---
blobname|The file name of the blob
blob|the JSON-escaped content of the blob

### repo list

Outer JSON name: **repolist**

Comprises an array of structures of the form

JSON name|Meaning
---|---
reponame|The name of the repo (xyz.git/ would have the reponame "xyz")
desc|The gitweb description
name|Gitweb owner name
email|Gitweb owner email
md5|md5 of the gitweb owner email
url|clone URL

### reflist

A list of refs from the repo

Outer JSON name: **reflist**

Comprises an array of structures of the form

JSON name|Meaning
---|---
name|The name of the ref, eg "refs/heads/master"
summary|A `{summary}` JSON struct, see below

The `{summary}` struct for a branch looks like

JSON name|Meaning
---|---
type|"commit"
time|The unix time of the commit
time_ofs|The timezone offset in minutes
oid_tree|The `{OID structure}` for the tree at this rev
oid|The `{OID structure}` for the commit at this rev
msg|The short commit log message
sig_commit|The `{signature structure}` for the committer
sig_author|The `{signature structure}` for the author

The `{summary}` struct for a tag looks like

JSON name|Meaning
---|---
type|"tag"
oid_tag|The `{OID structure}` for the tagged commit
type_tag|"commit"
msg_tag|The short commit log message
sig_tagger|The `{signature structure}` for the tagger

### log

This is a series of commits

Outer JSON name: **log**

Comprises an array of structures of the form

JSON name|Meaning
---|---
name|The `{OID structure}` for the commit at this rev
summary|A `{summary}` JSON struct, see below

The `{summary}` struct contains 

JSON name|Meaning
---|---
type|"commit"
time|The unix time of the commit
time_ofs|The timezone offset in minutes
oid_tree|The `{OID structure}` for the tree at this rev
oid|The `{OID structure}` for the commit at this rev
msg|The short commit log message
sig_commit|The `{signature structure}` for the committer
sig_author|The `{signature structure}` for the author

### blame

This information is provided after a "job" delivering the unannotated blob
for the file being blamed.

Outer JSON name: **blame** and **contrib**

The first `"blame": []` section comprises an array of structures of the form

JSON name|Meaning
---|---
ordinal|sort-order ordinal
orig_oid|The `{OID structure}` for the pre-patched content
final_oid|The `{OID structure}` for the post-patched content
sig_orig|The `{signature structure}` for the original content patch
sig_final|The `{signature structure}` for the final content patch
log_final|The short commit log message for the final content patch
op|Original patch filepath for the content (in the case of file rename or move)
ranges|An array of `{blame_range}` structures (see below)

The `{blame_range}` struct consists of

JSON name|Meaning
---|---
l|the number of lines
o|The start line number from the original file / revision
f|The start line number in the revision of the file being blamed

after the `"blame": []` array there is a second array `"contrib": []`, which
comprises an array of structures of the form:

JSON name|Meaning
---|---
l|The number of lines still remaining in the file from this contributor
o|The ordinal of the `blame: []` section entry whose `sig_final` member was by this contributor

NOTES: The first section is pre-sorted into order of first appearence in the
file being blamed.  It represents a patch which can have multiple discontiguous
line ranges in the version of the file currently being blamed, described in
its `"ranges": []` member.

After that information, the `"contrib": []` array is a list of all individual
contributors to the current file state, sorted by the number of lines of text
they have contributed.  If your libgit2 is recent enough (master or 0.28+) then
this list takes into account any .mailmap in the top level git in HEAD.

## Example commandline app

This commandline app takes two args

 - a directory where bare git repositories exist inside

 - a "url path" like /git/myrepo to specify what JSON is
   needed.

## Build

It's built along with the library

## Example usage

```
 $ jg2-example /srv/repositories /git/myrepo
 $ jg2-example /srv/repositories /git/myrepo?h=mybranch
 $ jg2-example /srv/repositories /git/myrepo/commit?id=<commit hash>
```


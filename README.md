A minimal-ish toolkit for downloading YouTube channels' newest videos.

Disclaimer: this software is highly specialized to my needs. If you like what I've got here as a starting point, I
encourage you to copy it all, throw it into your own git repo, and take ownership. It's only a couple hundred lines of
relatively straightforward C code. Then, you can tweak it to your liking, and don't need to depend on me to read your PR
or address your issue! :)

Ok, here's how to build and run this software.

First, make a few empty directories.

```shell
mkdir config data videos
```

Next, make a `config/channels.txt` file that contains a newline-separated list of YouTube channels names (without the
leading `@` that you would like to download. The order isn't relevant, it only controls the order that the channels are processed.
Here's an example:

```
3blue1brown
nprmusic
```

Next, allow `direnv` to set `$USER_ID` and `$GROUP_ID` to our own user and group id, so we can run the Docker containers
as ourselves:

```shell
direnv allow
```

Finally, start the Docker containers:

```shell
docker compose up
```

Here's what the software does:

1. Start a [POT provider server](https://github.com/Brainicism/bgutil-ytdlp-pot-provider) and wait for it to accept requests.
2. For each channel in `config/channels.txt`, download the videos were uploaded exactly 2 days ago with [`yt-dlp`](https://github.com/yt-dlp/yt-dlp).
3. Sleep until between 12:30am and 4:00am.
4. Repeat 2–4.

The videos are downloaded into `videos/CHANNEL/Season 01/TITLE`. You certainly may want to target a different directory,
perhaps one that doesn't have an awkward `Season 01` thing. I added that because I use Jellyfin as a media server, which
seems to require something like a "season number" directory between the "series name" (YouTube channel name) and
"episode name" (video name), so. Yeah.

I opted to download the videos that were uploaded exactly 2 days ago to give some time for SponsorBlock annotations
to roll in. This allows `yt-dlp` (via `ffmpeg`) to snip out sponsored segments and self-promotion. There are more
things you might want to snip.

The software writes a `data/archive.txt` file that can be deleted at any time, but which remembers the entire set of
YouTube videos ever downloaded. This allows us to run the software multiple times on the same day (say, during
development), and not have to worry about re-downloading things. It's just `yt-dlp`'s `--download-archive` flag.

I've omitted many details, but that's the gist of it. The source code tells the whole story. Go read all of the flags
we pass to `yt-dlp` as a starting point for customization. Have fun!

(This code would be public domain, but it depends on `bgutil-ytdlp-pot-provider`, which is GPL 3.0. Although
`bgutil-ytdlp-pot-provider` is run as a standalone executable, not linked into `ytcd`, I choose to license this code
as GPL 3.0 as well, for simplicity.)

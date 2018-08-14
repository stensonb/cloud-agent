/*
 * Copyright (c) 2018 Reyk Floeter <reyk@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fnmatch.h>
#include <fcntl.h>
#include <ctype.h>
#include <sha2.h>
#include <err.h>
#include <util.h>

#include "main.h"

int
opennebula(struct system_config *sc)
{
	FILE		*fp;
	const char	*delim = "\\\\\0", *errstr;
	char		*line = NULL, *k, *v, *p, q;
	char		*hname = NULL;
	size_t		 len, lineno = 0, i;
	int		 ret = -1;
	unsigned short	 unit;

	/* Return silently without error */
	if ((fp = fopen("/mnt/context.sh", "r")) == NULL)
		goto done;

	sc->sc_stack = "opennebula";

	while ((line = fparseln(fp, &len, &lineno,
	    delim, FPARSELN_UNESCALL)) != NULL) {
		/* key */
		k = line;

		/* a context always starts with this header */
		if (lineno == 1) {
			ret = strcmp(line,
			    "# Context variables generated by OpenNebula");
			if (ret != 0) {
				log_debug("%s: unsupported context", __func__);
				goto done;
			}
			free(line);
			continue;
		}
		line[strcspn(line, "#")] = '\0';

		/* value */
		if ((v = strchr(line, '=')) == NULL || *(v + 1) == '\0') {
			free(line);
			continue;
		}
		*v++ = '\0';

		/* value is quoted */
		q = *v;
		if (strspn(v, "\"'") == 0 || (p = strrchr(v, q)) == v) {
			free(line);
			continue;
		}
		*v++ = '\0';
		*p = '\0';

		/* continue if value is empty */
		if (*v == '\0') {
			free(line);
			continue;
		}

		log_debug("%s: %s = %s", __func__, k, v);

		if (strcasecmp("NETWORK", k) == 0) {
			if (strcasecmp("YES", v) == 0)
				sc->sc_network = 1;
			else if (strcasecmp("YES", v) == 0)
				sc->sc_network = 0;
		} else if (fnmatch("ETH*_*", k, 0) != FNM_NOMATCH) {
			/* Extract interface unit */
			if ((p = strdup(k + 3)) == NULL) {
				log_debug("%s: %s", __func__, k);
				goto done;
			}
			p[strcspn(p, "_")] = '\0';
			unit = strtonum(p, 0, UINT16_MAX, &errstr);
			free(p);
			if (errstr != NULL) {
				log_debug("%s: %s", __func__, k);
				goto done;
			}

			/* Get subkey */
			k += strcspn(k, "_") + 1;

			if (strcasecmp("DNS", k) == 0) {
				/* We don't support per-interface DNS */
				for (p = v; *p != '\0'; v = p) {
					p = v + strcspn(v, " \t");
					*p++ = '\0';
					if ((ret = agent_addnetaddr(sc, 0,
					    v, AF_UNSPEC, NET_DNS)) != 0)
						break;
				}
			} else if (strcasecmp("SEARCH_DOMAIN", k) == 0) {
				for (p = v; *p != '\0'; v = p) {
					p = v + strcspn(v, " \t");
					*p++ = '\0';
					if ((ret = agent_addnetaddr(sc, 0,
					    v, AF_UNSPEC, NET_DNS_DOMAIN)) != 0)
						break;
				}
			} else if (strcasecmp("IP", k) == 0) {
				ret = agent_addnetaddr(sc, unit,
				    v, AF_INET, NET_IP);
			} else if (strcasecmp("MASK", k) == 0) {
				ret = agent_addnetaddr(sc, unit,
				    v, AF_INET, NET_MASK);
			} else if (strcasecmp("GATEWAY", k) == 0) {
				ret = agent_addnetaddr(sc, unit,
				    v, AF_INET, NET_GATEWAY);
			} else if (strcasecmp("IP6", k) == 0) {
				ret = agent_addnetaddr(sc, unit,
				    v, AF_INET6, NET_IP);
			} else if (strcasecmp("GATEWAY6", k) == 0) {
				ret = agent_addnetaddr(sc, unit,
				    v, AF_INET6, NET_GATEWAY);
			} else if (strcasecmp("PREFIX_LENGTH", k) == 0) {
				ret = agent_addnetaddr(sc, unit,
				    v, AF_INET6, NET_PREFIX);
			} else if (strcasecmp("MAC", k) == 0) {
				if (unit == 0 && hname == NULL) {
					/* Fake a hostname using the mac */
					if ((hname = p = calloc(1,
					    strlen(v) + 3)) == NULL) {
						log_debug("%s: calloc",
						    __func__);
						goto done;
					}
					*p++ = 'v';
					*p++ = 'm';
					for (i = 0; i < strlen(v); i++) {
						if (!isalnum(v[i]))
							continue;
						*p++ = v[i];
					}
				}

				ret = agent_addnetaddr(sc, unit,
				    v, AF_UNSPEC, NET_MAC);
			} else if (strcasecmp("MTU", k) == 0) {
				ret = agent_addnetaddr(sc, unit,
				    v, AF_UNSPEC, NET_MTU);
			} else
				ret = 0;
			if (ret != 0) {
				log_debug("%s: failed to parse %s",
				    __func__, k);
				goto done;
			}
		} else if (strcasecmp("HOSTNAME", k) == 0) {
			if ((hname = strdup(v)) == NULL)
				log_warnx("failed to set hostname");
		} else if (strcasecmp("SSH_PUBLIC_KEY", k) == 0) {
			if (agent_addpubkey(sc, v, NULL) != 0)
				log_warnx("failed to set ssh pubkey");
		}

		free(line);
	}

	fclose(fp);
	fp = NULL;

	/*
	 * OpenNebula doesn't provide an instance id so we
	 * calculate one using the hash of the context file.
	 * This might break if the context is not consistent.
	 */
	if ((sc->sc_instance =
	    calloc(1, SHA256_DIGEST_STRING_LENGTH)) == NULL ||
	    SHA256File("/mnt/context.sh", sc->sc_instance) == NULL) {
		log_debug("%s: failed to calculate instance hash",
		    __func__);
		goto done;
	}
	log_debug("%s: context instance %s", __func__, sc->sc_instance);

	/* Even the hostname is optional */
	if (hname != NULL) {
		free(sc->sc_hostname);
		sc->sc_hostname = hname;
		log_debug("%s: hostname %s", __func__, hname);
	}

	line = NULL;
	ret = 0;

 done:
	if (fp != NULL)
		fclose(fp);
	free(line);
	return (ret);
}

# License and third-party notices

Raid Manager source, tools and documentation in this repository are distributed
under the GNU General Public License, version 2 or (at your option) any later
version, without warranty. The unmodified GPLv2 text is in LICENSE.

The module integrates with these separately distributed projects:

| Project | Use and observed notices |
| --- | --- |
| AzerothCore | APIs and source data; GPLv2-or-later notices in referenced headers. |
| mod-playerbots | Bot/BiS/scoring APIs and data; GPLv2 text and GPL/AGPL file notices. |
| mod-individual-progression | Progression/account classifier; source is not vendored or relicensed here. |

Upstream projects: [AzerothCore](https://github.com/azerothcore/azerothcore-wotlk),
[mod-playerbots](https://github.com/mod-playerbots/mod-playerbots).

The reference Playerbots StatsWeightCalculator implementation carries an AGPLv3-
or-later file notice. This module calls that API but does not vendor its implementation.
Use the appropriate license versions and preserve the notices/corresponding source
required by the complete server you distribute or operate. This module's LICENSE
does not override dependency licenses. GNU explains combination rules in its
[license compatibility guidance](https://www.gnu.org/licenses/license-compatibility.en.html).

Generated files in src/ and data/ are derived views of repository SQL, skill
mappings and a hashed proficiency seed. They retain source paths, hashes, line
numbers, loot associations and factual game identifiers. They are not newly
authored claims of ownership over upstream databases or game content. Upstream
author records are included in third_party/; individual row authors cannot be
reconstructed from the archived checkout.

No server database, credentials, player/account records, client DBC files, game
assets, third-party C++ implementation or binary is bundled. Game names are used
to describe interoperability; this project is not endorsed by their owners.

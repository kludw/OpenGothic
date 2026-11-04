#pragma once

// opengothic-rs (task 5.12): the C++ side of `og-parity sim-waynet`. With
// `--waynet-dump <path>` the World constructor writes, right after WayMatrix::buildIndex:
// the inputs buildIndex took from outside the waynet (each point's land ray, every
// ladder's box in mobsiById order), the waynet as buildIndex left it (points with
// ground positions, ladders and connections, edges, index orders, start/dead point),
// and the answer to each line of `--waynet-queries <path>`. Text lines; floats are their
// IEEE bits as 8 hex digits, names are hex bytes ("-" for empty). The format is read by
// crates/og-parity/src/sim_waynet.rs, which documents it.

#include <string_view>

class World;
class WayMatrix;

struct OgWaynetDump final {
  // Throws std::runtime_error (logged as "waynet dump: …") when a file can't be read or
  // written, a query line is malformed or a returned point isn't the matrix's; World's
  // constructor then also logs its generic "unable to load landscape mesh" and rethrows.
  static void write(World& world, const WayMatrix& matrix,
                    std::string_view dumpPath, std::string_view queriesPath);
  };

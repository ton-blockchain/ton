/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2017-2020 Telegram Systems LLP
*/
#include "download-archive-slice.hpp"
#include "td/utils/port/path.h"
#include "td/utils/overloaded.h"
#include "td/utils/Random.h"
#include <algorithm>
#include <map>
#include <set>

#include <ton/ton-tl.hpp>

namespace ton {

namespace validator {

namespace fullnode {

// Node quality tracking structure
struct NodeQuality {
  td::uint32 success_count = 0;
  td::uint32 failure_count = 0;
  td::uint32 archive_not_found_count = 0;
  td::uint32 consecutive_failures = 0;  // **NEW: Track consecutive failures**
  td::Timestamp last_success;
  td::Timestamp last_failure;
  td::Timestamp first_seen;
  double avg_speed = 0.0;
  double total_download_time = 0.0;
  
  // **NEW: Advanced metrics for explore-exploit strategy**
  td::uint32 total_attempts() const { return success_count + failure_count; }
  
  double success_rate() const {
    if (total_attempts() == 0) return 0.0;
    return double(success_count) / total_attempts();
  }
  
  double confidence_interval() const {
    if (total_attempts() == 0) return 1.0;  // High uncertainty for new nodes
    // Upper Confidence Bound calculation
    double exploration_factor = std::sqrt(2.0 * std::log(100.0) / total_attempts());
    return std::min(1.0, success_rate() + exploration_factor);
  }
  
  bool is_new_node() const {
    return total_attempts() < 3;  // Node with less than 3 attempts is considered "new"
  }
  
  // **IMPROVED: More conservative scoring system**
  double get_score() const {
    if (total_attempts() == 0) return 0.4;  // **REDUCED from 0.8 to 0.4 for unknown nodes**
    
    double base_score = success_rate();
    
    // **CONSERVATIVE EXPLORATION: Reduced bonuses for new nodes**
    double exploration_bonus = 0.0;
    if (is_new_node() && success_count > 0) {
      exploration_bonus = 0.1;  // **REDUCED from 0.3 to 0.1, only if had some success**
    } else if (total_attempts() < 10 && success_rate() >= 0.5) {
      exploration_bonus = 0.05;  // **REDUCED from 0.1 to 0.05, only for decent nodes**
    }
    
    // **ENHANCED TIME PENALTY: Penalize recent failures more heavily**
    double time_penalty = 0.0;
    if (failure_count > 0) {
      double time_since_failure = td::Timestamp::now().at() - last_failure.at();
      if (time_since_failure < 1800.0) {  // **EXTENDED from 600s to 1800s (30min)**
        // Base penalty for recent failures
        time_penalty = 0.3;  // **INCREASED from 0.2 to 0.3**
        
        // **NEW: Extra penalty for consecutive failures**
        if (consecutive_failures >= 3) {
          time_penalty += 0.2;  // Additional penalty for repeated failures
        }
        
        // **STRICTER: Less forgiveness for "archive not found"**
        if (archive_not_found_count > failure_count * 0.8) {
          time_penalty *= 0.7;  // **INCREASED from 0.5 to 0.7 (less forgiveness)**
        }
      }
    }
    
    // **NEW: Heavy penalty for nodes with very low success rates**
    double success_penalty = 0.0;
    if (total_attempts() >= 3 && success_rate() < 0.2) {
      success_penalty = 0.4;  // Heavy penalty for consistently failing nodes
    }
    
    // **SPEED BONUS: Unchanged**
    double speed_bonus = 0.0;
    if (success_count > 0 && avg_speed > 0) {
      speed_bonus = std::min(0.1, avg_speed / 10000000.0);  // Up to 0.1 bonus for 10MB/s+
    }
    
    return std::max(0.0, std::min(1.0, base_score + exploration_bonus - time_penalty - success_penalty + speed_bonus));
  }
  
  // **STRICTER: More aggressive blacklisting**
  bool is_blacklisted() const {
    // **NEW: Blacklist nodes with 3+ consecutive failures immediately**
    if (consecutive_failures >= 3) {
      double consecutive_blacklist_time = 1800.0;  // 30 minutes for consecutive failures
      return (td::Timestamp::now().at() - last_failure.at()) < consecutive_blacklist_time;
    }
    
    // **STRICTER: Reduced failure threshold from 5 to 3**
    if (failure_count < 3) return false;  
    
    // **STRICTER: Don't blacklist only if success rate > 50% (was 25%)**
    if (success_count * 2 > failure_count) return false;  
    
    // **EXTENDED: Longer blacklist times**
    double blacklist_time = 1800.0;  // **INCREASED from 900s to 1800s (30min)**
    if (archive_not_found_count > failure_count * 0.7) {
      blacklist_time = 900.0;  // **INCREASED from 300s to 900s (15min) for data issues**
    }
    
    // **NEW: Extra long blacklist for very unreliable nodes**
    if (success_rate() < 0.1 && total_attempts() >= 5) {
      blacklist_time = 3600.0;  // 1 hour for extremely unreliable nodes
    }
    
    return (td::Timestamp::now().at() - last_failure.at()) < blacklist_time;
  }
  
  // **NEW: Helper to update failure tracking**
  void record_failure() {
    failure_count++;
    consecutive_failures++;
    last_failure = td::Timestamp::now();
  }
  
  // **NEW: Helper to update success tracking**
  void record_success() {
    success_count++;
    consecutive_failures = 0;  // Reset consecutive failures on success
    last_success = td::Timestamp::now();
  }
};

// Static node quality tracking (shared across instances)
static std::map<adnl::AdnlNodeIdShort, NodeQuality> node_qualities_;
static std::set<adnl::AdnlNodeIdShort> active_attempts_;
static td::uint32 strategy_attempt_ = 0;

// **NEW: Block-level data availability tracking**
struct BlockAvailability {
  td::uint32 not_found_count = 0;
  td::uint32 total_attempts = 0;
  td::Timestamp first_attempt;
  td::Timestamp last_not_found;
  
  bool is_likely_unavailable() const {
    if (total_attempts < 3) return false;  // Need some attempts first
    double not_found_rate = double(not_found_count) / total_attempts;
    bool recent_failures = (td::Timestamp::now().at() - last_not_found.at()) < 300.0;  // 5min
    return not_found_rate > 0.8 && recent_failures;  // 80%+ not found rate
  }
  
  td::uint32 recommended_delay() const {
    if (!is_likely_unavailable()) return 0;
    return std::min(300u, not_found_count * 30);  // Up to 5min delay, 30s per failure
  }
};

static std::map<BlockSeqno, BlockAvailability> block_availability_;

DownloadArchiveSlice::DownloadArchiveSlice(
    BlockSeqno masterchain_seqno, ShardIdFull shard_prefix, std::string tmp_dir, adnl::AdnlNodeIdShort local_id,
    overlay::OverlayIdShort overlay_id, adnl::AdnlNodeIdShort download_from, td::Timestamp timeout,
    td::actor::ActorId<ValidatorManagerInterface> validator_manager, td::actor::ActorId<adnl::AdnlSenderInterface> rldp,
    td::actor::ActorId<overlay::Overlays> overlays, td::actor::ActorId<adnl::Adnl> adnl,
    td::actor::ActorId<adnl::AdnlExtClient> client, td::Promise<std::string> promise)
    : masterchain_seqno_(masterchain_seqno)
    , shard_prefix_(shard_prefix)
    , tmp_dir_(std::move(tmp_dir))
    , local_id_(local_id)
    , overlay_id_(overlay_id)
    , download_from_(download_from)
    , timeout_(timeout)
    , validator_manager_(validator_manager)
    , rldp_(rldp)
    , overlays_(overlays)
    , adnl_(adnl)
    , client_(client)
    , promise_(std::move(promise)) {
}

void DownloadArchiveSlice::abort_query(td::Status reason) {
  if (promise_) {
    LOG(WARNING) << "🚫 Failed to download archive slice #" << masterchain_seqno_ 
                 << " for shard " << shard_prefix_.to_str() << ": " << reason;
    promise_.set_error(std::move(reason));
    if (!fd_.empty()) {
      td::unlink(tmp_name_).ensure();
      fd_.close();
    }
  }
  active_attempts_.erase(download_from_);
  stop();
}

void DownloadArchiveSlice::alarm() {
  abort_query(td::Status::Error(ErrorCode::timeout, "timeout"));
}

void DownloadArchiveSlice::finish_query() {
  if (promise_) {
    LOG(INFO) << "✅ Successfully downloaded archive slice #" << masterchain_seqno_ 
              << " " << shard_prefix_.to_str() << ": " << td::format::as_size(offset_);
    
    // **ENHANCED: Update node quality with detailed statistics**
    if (!download_from_.is_zero()) {
      auto& quality = node_qualities_[download_from_];
      quality.record_success();  // **UPDATED: Use new helper method**
      
      // **NEW: Calculate and update download speed**
      double download_time = prev_logged_timer_.elapsed() > 0 ? prev_logged_timer_.elapsed() : 1.0;
      double current_speed = static_cast<double>(offset_) / download_time;  // bytes per second
      
      if (quality.success_count == 1) {
        quality.avg_speed = current_speed;
        quality.total_download_time = download_time;
      } else {
        // Update running average speed
        quality.total_download_time += download_time;
        quality.avg_speed = (quality.avg_speed * (quality.success_count - 1) + current_speed) / quality.success_count;
      }
      
      LOG(INFO) << "✅ Node " << download_from_ << " SUCCESS"
                << " | Score: " << quality.get_score()
                << " | Success Rate: " << (quality.success_rate() * 100) << "%"
                << " | Attempts: " << quality.total_attempts()
                << " | Speed: " << td::format::as_size(static_cast<td::uint64>(current_speed)) << "/s"
                << " | Avg Speed: " << td::format::as_size(static_cast<td::uint64>(quality.avg_speed)) << "/s";
    }
    
    promise_.set_value(std::move(tmp_name_));
    fd_.close();
  }
  active_attempts_.erase(download_from_);
  stop();
}

// Helper function to select best nodes with explore-exploit strategy
std::vector<adnl::AdnlNodeIdShort> select_best_nodes(const std::vector<adnl::AdnlNodeIdShort>& nodes, td::uint32 count) {
  if (nodes.empty()) return {};
  
  std::vector<std::pair<double, adnl::AdnlNodeIdShort>> all_nodes;
  std::vector<std::pair<double, adnl::AdnlNodeIdShort>> high_quality_nodes;  // **NEW: Track high-quality nodes**
  std::vector<std::pair<double, adnl::AdnlNodeIdShort>> medium_nodes;
  std::vector<std::pair<double, adnl::AdnlNodeIdShort>> new_nodes;
  
  td::uint32 new_count = 0;
  td::uint32 experienced_count = 0;
  td::uint32 blacklisted_count = 0;
  td::uint32 high_quality_count = 0;
  
  for (auto& node : nodes) {
    auto it = node_qualities_.find(node);
    
    if (it == node_qualities_.end()) {
      // **EXPLORATION: New unknown nodes**
      double new_node_score = 0.6;  // Moderate score for new nodes
      all_nodes.emplace_back(new_node_score, node);
      new_nodes.emplace_back(new_node_score, node);
      new_count++;
      
      // Initialize tracking for new node
      auto& quality = node_qualities_[node];
      if (quality.first_seen.at() == 0.0) {
        quality.first_seen = td::Timestamp::now();
        LOG(INFO) << "🆕 Discovered new node " << node;
      }
      
    } else {
      if (it->second.is_blacklisted()) {
        blacklisted_count++;
        LOG(INFO) << "🚫 Skipping blacklisted node " << node 
                  << " (failures: " << it->second.failure_count << ")";
        continue;  // Skip blacklisted nodes
      }
      
      double score = it->second.get_score();
      
      // **ENHANCED: More strict filtering for low quality nodes**
      if (score < 0.2 && it->second.total_attempts() >= 2) {  // **INCREASED threshold from 0.1 to 0.2**
        blacklisted_count++;
        LOG(WARNING) << "🚫 Filtering low-quality node " << node 
                     << " | Score: " << score
                     << " | Success Rate: " << (it->second.success_rate() * 100) << "%"
                     << " | Attempts: " << it->second.total_attempts()
                     << " | Consecutive Failures: " << it->second.consecutive_failures;
        continue;  // Skip very low quality nodes
      }
      
      // **NEW: Additional filtering for nodes with consecutive failures**
      if (it->second.consecutive_failures >= 2 && it->second.success_rate() < 0.3) {
        blacklisted_count++;
        LOG(WARNING) << "🚫 Filtering node with consecutive failures " << node 
                     << " | Consecutive Failures: " << it->second.consecutive_failures
                     << " | Success Rate: " << (it->second.success_rate() * 100) << "%";
        continue;
      }
      
      all_nodes.emplace_back(score, node);
      
      // **ENHANCED: Categorize nodes by quality with better logic**
      if (it->second.success_rate() >= 0.7 && it->second.total_attempts() >= 2) {
        high_quality_nodes.emplace_back(score, node);
        high_quality_count++;
        LOG(INFO) << "⭐ High-quality node found: " << node 
                  << " (score=" << score << ", success_rate=" << (it->second.success_rate() * 100) << "%)";
      } else if (it->second.is_new_node() || (score >= 0.3 && it->second.success_rate() >= 0.3)) {
        // NEW: Only medium nodes if they have some success OR are new
        medium_nodes.emplace_back(score, node);
        LOG(INFO) << "🔶 Medium-quality node: " << node 
                  << " (score=" << score << ", success_rate=" << (it->second.success_rate() * 100) << "%)";
      } else {
        // **NEW: Log nodes that are being filtered out**
        LOG(INFO) << "🔻 Low-quality node available but deprioritized: " << node 
                  << " (score=" << score << ", success_rate=" << (it->second.success_rate() * 100) << "%)";
      }
      
      if (it->second.is_new_node()) {
        new_count++;
      } else {
        experienced_count++;
      }
    }
  }
  
  // **PRIORITIZED SELECTION STRATEGY**
  std::vector<adnl::AdnlNodeIdShort> result;
  td::uint32 selected_count = std::min(count, static_cast<td::uint32>(all_nodes.size()));
  
  if (all_nodes.empty()) {
    LOG(WARNING) << "❌ No available nodes (blacklisted: " << blacklisted_count << ")";
    return result;
  }
  
  LOG(INFO) << "🎯 SELECTION ANALYSIS - Total: " << nodes.size() 
            << " | High-Quality: " << high_quality_count 
            << " | Medium: " << medium_nodes.size()
            << " | New: " << new_count 
            << " | Blacklisted: " << blacklisted_count;
  
  // **STRATEGY 1: PRIORITIZE HIGH-QUALITY NODES**
  if (!high_quality_nodes.empty()) {
    // Sort high-quality nodes by score
    std::sort(high_quality_nodes.begin(), high_quality_nodes.end(), 
              [](const auto& a, const auto& b) { return a.first > b.first; });
    
    // Select at least 60% from high-quality nodes
    td::uint32 high_quality_slots = std::max(1u, static_cast<td::uint32>(selected_count * 0.6));
    high_quality_slots = std::min(high_quality_slots, static_cast<td::uint32>(high_quality_nodes.size()));
    
    for (td::uint32 i = 0; i < high_quality_slots; i++) {
      result.push_back(high_quality_nodes[i].second);
      auto it = node_qualities_.find(high_quality_nodes[i].second);
      LOG(INFO) << "✅ PRIORITY SELECT: " << high_quality_nodes[i].second 
                << " | Score: " << high_quality_nodes[i].first
                << " | Success Rate: " << (it->second.success_rate() * 100) << "%"
                << " | Attempts: " << it->second.total_attempts();
    }
    
    selected_count -= high_quality_slots;
  }
  
  // **STRATEGY 2: FILL REMAINING SLOTS WITH EXPLORATION/MEDIUM NODES**
  if (selected_count > 0) {
    std::vector<std::pair<double, adnl::AdnlNodeIdShort>> remaining_candidates;
    
    // Combine medium and new nodes for remaining slots
    remaining_candidates.insert(remaining_candidates.end(), medium_nodes.begin(), medium_nodes.end());
    remaining_candidates.insert(remaining_candidates.end(), new_nodes.begin(), new_nodes.end());
    
    // Sort remaining candidates by score
    std::sort(remaining_candidates.begin(), remaining_candidates.end(), 
              [](const auto& a, const auto& b) { return a.first > b.first; });
    
    for (td::uint32 i = 0; i < std::min(selected_count, static_cast<td::uint32>(remaining_candidates.size())); i++) {
      result.push_back(remaining_candidates[i].second);
      
      auto it = node_qualities_.find(remaining_candidates[i].second);
      if (it != node_qualities_.end()) {
        LOG(INFO) << "🔍 EXPLORE SELECT: " << remaining_candidates[i].second 
                  << " | Score: " << remaining_candidates[i].first
                  << " | Success Rate: " << (it->second.success_rate() * 100) << "%"
                  << " | Attempts: " << it->second.total_attempts();
      } else {
        LOG(INFO) << "🆕 NEW NODE SELECT: " << remaining_candidates[i].second 
                  << " | Score: " << remaining_candidates[i].first;
      }
    }
  }
  
  // **FALLBACK: If no nodes selected, pick the best available**
  if (result.empty() && !all_nodes.empty()) {
    std::sort(all_nodes.begin(), all_nodes.end(), 
              [](const auto& a, const auto& b) { return a.first > b.first; });
    
    // **STRICTER: Even in fallback, maintain high standards**
    std::vector<std::pair<double, adnl::AdnlNodeIdShort>> acceptable_fallback;
    for (auto& node_pair : all_nodes) {
      // **INCREASED threshold from 0.1 to 0.25 for fallback**
      if (node_pair.first >= 0.25) {  
        auto it = node_qualities_.find(node_pair.second);
        // Additional check: no more than 2 consecutive failures
        if (it == node_qualities_.end() || it->second.consecutive_failures <= 2) {
          acceptable_fallback.push_back(node_pair);
        }
      }
    }
    
    if (!acceptable_fallback.empty()) {
      result.push_back(acceptable_fallback[0].second);
      LOG(WARNING) << "⚠️ FALLBACK SELECT (acceptable): " << acceptable_fallback[0].second 
                   << " | Score: " << acceptable_fallback[0].first;
    } else {
      // **NEW: No last resort selection - fail gracefully instead**
      LOG(ERROR) << "🚫 NO ACCEPTABLE NODES AVAILABLE - All nodes are too unreliable!"
                 << " | Total candidates: " << all_nodes.size()
                 << " | Blacklisted: " << blacklisted_count;
      // Don't select any node - let the system request more candidates
    }
  }
  
  if (result.empty()) {
    LOG(ERROR) << "💥 NO NODES SELECTED! This should not happen!";
  }
  
  return result;
}

void DownloadArchiveSlice::start_up() {
  alarm_timestamp() = timeout_;

  auto R = td::mkstemp(tmp_dir_);
  if (R.is_error()) {
    abort_query(R.move_as_error_prefix("failed to open temp file: "));
    return;
  }
  auto r = R.move_as_ok();
  fd_ = std::move(r.first);
  tmp_name_ = std::move(r.second);

  // **NEW: Check block-level data availability**
  auto& block_avail = block_availability_[masterchain_seqno_];
  if (block_avail.first_attempt.at() == 0.0) {
    block_avail.first_attempt = td::Timestamp::now();
  }
  
  if (block_avail.is_likely_unavailable()) {
    td::uint32 delay = block_avail.recommended_delay();
    LOG(WARNING) << "⏳ Block #" << masterchain_seqno_ << " likely unavailable"
                 << " | NotFound: " << block_avail.not_found_count 
                 << "/" << block_avail.total_attempts 
                 << " | Delaying " << delay << "s";
    
    // Delay download attempt
    alarm_timestamp() = td::Timestamp::in(static_cast<double>(delay));
    return;
  }

  LOG(INFO) << "📦 Starting optimized download of archive slice #" << masterchain_seqno_ 
            << " " << shard_prefix_.to_str();

  if (download_from_.is_zero() && client_.empty()) {
    // **NEW: First try to use known high-quality nodes**
    std::vector<adnl::AdnlNodeIdShort> known_good_nodes;
    for (auto& pair : node_qualities_) {
      if (!pair.second.is_blacklisted() && 
          pair.second.success_rate() >= 0.7 && 
          pair.second.total_attempts() >= 2) {
        known_good_nodes.push_back(pair.first);
      }
    }
    
    if (!known_good_nodes.empty()) {
      // **NEW: 80% use known good nodes, 20% explore new nodes**
      bool use_known_node = (td::Random::fast(1, 100) <= 80);  // 80% probability
      
      if (use_known_node) {
        // Sort by score and pick the best
        std::sort(known_good_nodes.begin(), known_good_nodes.end(), 
                  [](const adnl::AdnlNodeIdShort& a, const adnl::AdnlNodeIdShort& b) {
                    auto it_a = node_qualities_.find(a);
                    auto it_b = node_qualities_.find(b);
                    return it_a->second.get_score() > it_b->second.get_score();
                  });
        
        // **NEW: Add some randomness among top nodes to avoid overusing single node**
        td::uint32 top_nodes_count = std::min(3u, static_cast<td::uint32>(known_good_nodes.size()));
        td::uint32 selected_idx = td::Random::fast(0, static_cast<td::int32>(top_nodes_count - 1));
        
        auto best_known = known_good_nodes[selected_idx];
        auto it = node_qualities_.find(best_known);
        LOG(INFO) << "🏆 PRIORITIZING known high-quality node: " << best_known
                  << " | Score: " << it->second.get_score()
                  << " | Success Rate: " << (it->second.success_rate() * 100) << "%"
                  << " | Attempts: " << it->second.total_attempts()
                  << " | Rank: " << (selected_idx + 1) << "/" << known_good_nodes.size();
        
        got_node_to_download(best_known);
        return;
      } else {
        LOG(INFO) << "🎲 EXPLORATION MODE: Skipping " << known_good_nodes.size() 
                  << " known good nodes to explore new options";
      }
    } else {
      LOG(INFO) << "🔍 No known high-quality nodes available, requesting from overlay...";
    }

    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), this](td::Result<std::vector<adnl::AdnlNodeIdShort>> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &DownloadArchiveSlice::abort_query, R.move_as_error());
      } else {
        auto vec = R.move_as_ok();
        if (vec.size() == 0) {
          td::actor::send_closure(SelfId, &DownloadArchiveSlice::abort_query,
                                  td::Status::Error(ErrorCode::notready, "no nodes"));
        } else {
          // **ENHANCED: Smart explore-exploit node selection**
          LOG(INFO) << "🔍 Starting node selection from " << vec.size() << " candidates";
          auto best_nodes = select_best_nodes(vec, std::min(static_cast<td::uint32>(vec.size()), static_cast<td::uint32>(3)));
          
          if (!best_nodes.empty()) {
            LOG(INFO) << "🎯 Smart selection completed from " << vec.size() << " candidates, chose: " << best_nodes[0];
            td::actor::send_closure(SelfId, &DownloadArchiveSlice::got_node_to_download, best_nodes[0]);
          } else {
            // **NEW: If all nodes are blacklisted, request more nodes**
            LOG(WARNING) << "⚠️ All initial nodes blacklisted or filtered, requesting more candidates...";
            // Request more nodes when initial selection fails
            auto P2 = td::PromiseCreator::lambda([SelfId](td::Result<std::vector<adnl::AdnlNodeIdShort>> R2) {
              if (R2.is_error()) {
                td::actor::send_closure(SelfId, &DownloadArchiveSlice::abort_query, R2.move_as_error());
              } else {
                auto vec2 = R2.move_as_ok();
                if (!vec2.empty()) {
                  LOG(INFO) << "🔄 Fallback to any available node from " << vec2.size() << " candidates";
                  td::actor::send_closure(SelfId, &DownloadArchiveSlice::got_node_to_download, vec2[0]);
                } else {
                  td::actor::send_closure(SelfId, &DownloadArchiveSlice::abort_query,
                                          td::Status::Error(ErrorCode::notready, "no fallback nodes"));
                }
              }
            });
            // Request more nodes as fallback - call directly, not via actor
            request_more_nodes(std::move(P2));
            return;
          }
        }
      }
    });

    // **OPTIMIZATION: Request more nodes for better selection**
    td::actor::send_closure(overlays_, &overlay::Overlays::get_overlay_random_peers, local_id_, overlay_id_, 6,
                            std::move(P));
  } else {
    got_node_to_download(download_from_);
  }
}

void DownloadArchiveSlice::got_node_to_download(adnl::AdnlNodeIdShort download_from) {
  download_from_ = download_from;
  active_attempts_.insert(download_from);

  // **ENHANCED: Check if node is blacklisted with detailed info**
  auto it = node_qualities_.find(download_from);
  if (it != node_qualities_.end() && it->second.is_blacklisted()) {
    LOG(WARNING) << "❌ Node " << download_from << " is BLACKLISTED"
                 << " | Score: " << it->second.get_score()
                 << " | Success Rate: " << (it->second.success_rate() * 100) << "%"
                 << " | Attempts: " << it->second.total_attempts()
                 << " | Recent Failures: " << it->second.failure_count
                 << " | Consecutive Failures: " << it->second.consecutive_failures;
    abort_query(td::Status::Error(ErrorCode::notready, "node blacklisted"));
    return;
  }

  // **NEW: Log node selection details**
  if (it != node_qualities_.end()) {
    LOG(INFO) << "🚀 Using node " << download_from 
              << " | Score: " << it->second.get_score()
              << " | Success Rate: " << (it->second.success_rate() * 100) << "%"
              << " | Attempts: " << it->second.total_attempts()
              << " | Type: " << (it->second.is_new_node() ? "NEW" : "EXPERIENCED");
  } else {
    LOG(INFO) << "🆕 Using completely unknown node " << download_from;
  }

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &DownloadArchiveSlice::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &DownloadArchiveSlice::got_archive_info, R.move_as_ok());
    }
  });

  td::BufferSlice q;
  if (shard_prefix_.is_masterchain()) {
    q = create_serialize_tl_object<ton_api::tonNode_getArchiveInfo>(masterchain_seqno_);
  } else {
    q = create_serialize_tl_object<ton_api::tonNode_getShardArchiveInfo>(masterchain_seqno_,
                                                                         create_tl_shard_id(shard_prefix_));
  }
  if (client_.empty()) {
    // **OPTIMIZATION: Shorter timeout for faster failure detection**
    td::actor::send_closure(overlays_, &overlay::Overlays::send_query, download_from_, local_id_, overlay_id_,
                            "get_archive_info", std::move(P), td::Timestamp::in(2.0), std::move(q));
  } else {
    td::actor::send_closure(client_, &adnl::AdnlExtClient::send_query, "get_archive_info",
                            create_serialize_tl_object_suffix<ton_api::tonNode_query>(std::move(q)),
                            td::Timestamp::in(1.0), std::move(P));
  }
}

void DownloadArchiveSlice::got_archive_info(td::BufferSlice data) {
  auto F = fetch_tl_object<ton_api::tonNode_ArchiveInfo>(std::move(data), true);
  if (F.is_error()) {
    // **ENHANCED: Track node failure with detailed statistics**
    auto& quality = node_qualities_[download_from_];
    quality.record_failure();  // **UPDATED: Use new helper method**
    
                    LOG(WARNING) << "❌ Node " << download_from_ << " FAILED to parse ArchiveInfo"
                << " | Score: " << quality.get_score()
                << " | Success Rate: " << (quality.success_rate() * 100) << "%"
                << " | Attempts: " << quality.total_attempts()
                << " | Consecutive Failures: " << quality.consecutive_failures;
    
    abort_query(F.move_as_error_prefix("failed to parse ArchiveInfo answer"));
    return;
  }
  auto f = F.move_as_ok();

  bool fail = false;
  ton_api::downcast_call(*f.get(), td::overloaded(
                                       [&](const ton_api::tonNode_archiveNotFound &obj) {
                                         // **ENHANCED: Track specific failure type**
                                         auto& quality = node_qualities_[download_from_];
                                         quality.record_failure();  // **UPDATED: Use new helper method**
                                         quality.archive_not_found_count++;
                                         
                                         LOG(WARNING) << "❌ Node " << download_from_ << " ARCHIVE NOT FOUND"
                                                      << " | Score: " << quality.get_score()
                                                      << " | Success Rate: " << (quality.success_rate() * 100) << "%"
                                                      << " | Attempts: " << quality.total_attempts()
                                                      << " | NotFound: " << quality.archive_not_found_count
                                                      << " | Consecutive Failures: " << quality.consecutive_failures;
                                         
                                         abort_query(td::Status::Error(ErrorCode::notready, "remote db not found"));
                                         fail = true;
                                       },
                                       [&](const ton_api::tonNode_archiveInfo &obj) { archive_id_ = obj.id_; }));
  if (fail) {
    return;
  }

  // **NEW: Record download start time for speed calculation**
  prev_logged_timer_ = td::Timer();  // Reset timer at start of actual download
  LOG(INFO) << "📦 Found archive info from " << download_from_ << ", starting download";
  get_archive_slice();
}

void DownloadArchiveSlice::get_archive_slice() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &DownloadArchiveSlice::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &DownloadArchiveSlice::got_archive_slice, R.move_as_ok());
    }
  });

  auto q = create_serialize_tl_object<ton_api::tonNode_getArchiveSlice>(archive_id_, offset_, slice_size());
  if (client_.empty()) {
    // **OPTIMIZATION: Longer timeout for actual data transfer**
    td::actor::send_closure(overlays_, &overlay::Overlays::send_query_via, download_from_, local_id_, overlay_id_,
                            "get_archive_slice", std::move(P), td::Timestamp::in(25.0), std::move(q),
                            slice_size() + 1024, rldp_);
  } else {
    td::actor::send_closure(client_, &adnl::AdnlExtClient::send_query, "get_archive_slice",
                            create_serialize_tl_object_suffix<ton_api::tonNode_query>(std::move(q)),
                            td::Timestamp::in(20.0), std::move(P));
  }
}

void DownloadArchiveSlice::got_archive_slice(td::BufferSlice data) {
  auto R = fd_.write(data.as_slice());
  if (R.is_error()) {
    abort_query(R.move_as_error_prefix("failed to write temp file: "));
    return;
  }
  if (R.move_as_ok() != data.size()) {
    abort_query(td::Status::Error(ErrorCode::error, "short write to temp file"));
    return;
  }

  offset_ += data.size();

  // **OPTIMIZATION: Enhanced progress logging**
  double elapsed = prev_logged_timer_.elapsed();
  if (elapsed > 3.0) {  // Log every 3 seconds
    prev_logged_timer_ = td::Timer();
    auto speed = static_cast<double>(offset_ - prev_logged_sum_) / elapsed;
    LOG(INFO) << "⬇️  Downloading archive slice #" << masterchain_seqno_ 
              << " " << shard_prefix_.to_str() << ": " << td::format::as_size(offset_)
              << " (" << td::format::as_size(static_cast<td::uint64>(speed)) << "/s)";
    prev_logged_sum_ = offset_;
  }

  if (data.size() < slice_size()) {
    finish_query();
  } else {
    get_archive_slice();
  }
}

// **NEW: Method to request more nodes when initial selection fails**
void DownloadArchiveSlice::request_more_nodes(td::Promise<std::vector<adnl::AdnlNodeIdShort>> promise) {
  LOG(INFO) << "🔄 Requesting additional nodes due to blacklist situation";
  
  // Request double the usual amount for better chances
  td::actor::send_closure(overlays_, &overlay::Overlays::get_overlay_random_peers, local_id_, overlay_id_, 12,
                          std::move(promise));
}

}  // namespace fullnode

}  // namespace validator

}  // namespace ton

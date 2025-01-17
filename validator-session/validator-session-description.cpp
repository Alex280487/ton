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
#include "validator-session.hpp"
#include "td/utils/Random.h"
#include "td/utils/crypto.h"
#include "validator-session-description.hpp"

namespace ton {

namespace validatorsession {

ValidatorSessionDescriptionImpl::Source::Source(ValidatorSessionNode &node) {
  encryptor = node.pub_key.create_encryptor().move_as_ok();
  weight = node.weight;
  id = node.pub_key.compute_short_id();
  full_id = node.pub_key;
  adnl_id = node.adnl_id;
}

ValidatorSessionDescriptionImpl::ValidatorSessionDescriptionImpl(ValidatorSessionOptions opts,
                                                                 std::vector<ValidatorSessionNode> &nodes,
                                                                 PublicKeyHash local_id)
    : opts_(std::move(opts)) {
  td::uint32 size = static_cast<td::uint32>(nodes.size());
  ValidatorWeight total_weight = 0;
  for (td::uint32 i = 0; i < size; i++) {
    sources_.emplace_back(nodes[i]);
    total_weight += sources_[i].weight;
    CHECK(rev_sources_.find(sources_[i].id) == rev_sources_.end());
    rev_sources_[sources_[i].id] = i;
  }
  total_weight_ = total_weight;
  cutoff_weight_ = (total_weight * 2) / 3 + 1;
  auto it = rev_sources_.find(local_id);
  CHECK(it != rev_sources_.end());
  self_idx_ = it->second;

  for (auto &el : cache_) {
    Cached v{nullptr};
    el.store(v, std::memory_order_relaxed);
  }
}

td::int32 ValidatorSessionDescriptionImpl::get_node_priority(td::uint32 src_idx, td::uint32 round) const {
  round %= get_total_nodes();
  if (src_idx < round) {
    src_idx += get_total_nodes();
  }
  if (src_idx - round < opts_.round_candidates) {
    return src_idx - round;
  }
  return -1;
}

td::uint32 ValidatorSessionDescriptionImpl::get_max_priority() const {
  return opts_.round_candidates - 1;
}

td::uint32 ValidatorSessionDescriptionImpl::get_node_by_priority(td::uint32 round, td::uint32 priority) const {
  CHECK(priority <= get_max_priority());
  return (round + priority) % get_total_nodes();
}

ValidatorSessionCandidateId ValidatorSessionDescriptionImpl::candidate_id(
    td::uint32 src_idx, ValidatorSessionRootHash root_hash, ValidatorSessionFileHash file_hash,
    ValidatorSessionCollatedDataFileHash collated_data_file_hash) const {
  auto obj = create_tl_object<ton_api::validatorSession_candidateId>(get_source_id(src_idx).tl(), root_hash, file_hash,
                                                                     collated_data_file_hash);
  return get_tl_object_sha_bits256(obj);
}

td::Status ValidatorSessionDescriptionImpl::check_signature(ValidatorSessionRootHash root_hash,
                                                            ValidatorSessionFileHash file_hash, td::uint32 src_idx,
                                                            td::Slice signature) const {
  auto obj = create_tl_object<ton_api::ton_blockId>(root_hash, file_hash);
  auto S = serialize_tl_object(obj, true);

  return sources_[src_idx].encryptor->check_signature(S.as_slice(), signature);
}

td::Status ValidatorSessionDescriptionImpl::check_approve_signature(ValidatorSessionRootHash root_hash,
                                                                    ValidatorSessionFileHash file_hash,
                                                                    td::uint32 src_idx, td::Slice signature) const {
  auto obj = create_tl_object<ton_api::ton_blockIdApprove>(root_hash, file_hash);
  auto S = serialize_tl_object(obj, true);

  return sources_[src_idx].encryptor->check_signature(S.as_slice(), signature);
}

std::vector<PublicKeyHash> ValidatorSessionDescriptionImpl::export_nodes() const {
  std::vector<PublicKeyHash> v;
  v.resize(get_total_nodes());
  for (td::uint32 i = 0; i < get_total_nodes(); i++) {
    v[i] = sources_[i].id;
  }
  return v;
}

std::vector<catchain::CatChainNode> ValidatorSessionDescriptionImpl::export_catchain_nodes() const {
  std::vector<catchain::CatChainNode> v;
  v.resize(get_total_nodes());
  for (td::uint32 i = 0; i < get_total_nodes(); i++) {
    v[i].pub_key = sources_[i].full_id;
    v[i].adnl_id = sources_[i].adnl_id;
  }
  return v;
}

std::vector<PublicKey> ValidatorSessionDescriptionImpl::export_full_nodes() const {
  std::vector<PublicKey> v;
  v.resize(get_total_nodes());
  for (td::uint32 i = 0; i < get_total_nodes(); i++) {
    v[i] = sources_[i].full_id;
  }
  return v;
}

double ValidatorSessionDescriptionImpl::get_delay(td::uint32 priority) const {
  return ((sources_.size() >= 5 ? 0 : 1) + priority) * opts_.next_candidate_delay;
}

td::uint32 ValidatorSessionDescriptionImpl::get_vote_for_author(td::uint32 attempt_seqno) const {
  return attempt_seqno % get_total_nodes();
}

const ValidatorSessionDescription::RootObject *ValidatorSessionDescriptionImpl::get_by_hash(HashType hash,
                                                                                            bool allow_temp) const {
  auto x = hash % cache_size;

  return cache_[x].load(std::memory_order_relaxed).ptr;
}

HashType ValidatorSessionDescriptionImpl::compute_hash(td::Slice data) const {
  return td::crc32c(data);
}

void ValidatorSessionDescriptionImpl::update_hash(const RootObject *obj, HashType hash) {
  if (!is_persistent(obj)) {
    return;
  }
  auto x = hash % cache_size;
  Cached p{obj};
  cache_[x].store(p, std::memory_order_relaxed);
}

void *ValidatorSessionDescriptionImpl::alloc(size_t size, size_t align, bool temp) {
  return (temp ? mem_temp_ : mem_perm_).alloc(size, align);
}

bool ValidatorSessionDescriptionImpl::is_persistent(const void *ptr) const {
  return mem_perm_.contains(ptr);
}

std::unique_ptr<ValidatorSessionDescription> ValidatorSessionDescription::create(
    ValidatorSessionOptions opts, std::vector<ValidatorSessionNode> &nodes, PublicKeyHash local_id) {
  return std::make_unique<ValidatorSessionDescriptionImpl>(std::move(opts), nodes, local_id);
}

ValidatorSessionDescriptionImpl::MemPool::MemPool(size_t chunk_size) : chunk_size_(chunk_size) {
}

ValidatorSessionDescriptionImpl::MemPool::~MemPool() {
  for (auto &v : data_) {
    delete[] v;
  }
}

void *ValidatorSessionDescriptionImpl::MemPool::alloc(size_t size, size_t align) {
  CHECK(align && !(align & (align - 1)));  // align should be a power of 2
  CHECK(size + align <= chunk_size_);
  auto get_padding = [&](const uint8_t* ptr) {
    return (-(size_t)ptr) & (align - 1);
  };
  while (true) {
    size_t idx = ptr_ / chunk_size_;
    if (idx < data_.size()) {
      auto ptr = data_[idx] + (ptr_ % chunk_size_);
      ptr_ += get_padding(ptr);
      ptr += get_padding(ptr);
      ptr_ += size;
      if (ptr_ <= data_.size() * chunk_size_) {
        return static_cast<void *>(ptr);
      } else {
        ptr_ = data_.size() * chunk_size_;
      }
    }
    data_.push_back(new td::uint8[chunk_size_]);
  }
}

void ValidatorSessionDescriptionImpl::MemPool::clear() {
  while (data_.size() > 1) {
    delete[] data_.back();
    data_.pop_back();
  }
  ptr_ = 0;
}

bool ValidatorSessionDescriptionImpl::MemPool::contains(const void* ptr) const {
  if (ptr == nullptr) {
    return true;
  }
  for (auto &v : data_) {
    if (ptr >= v && ptr <= v + chunk_size_) {
      return true;
    }
  }
  return false;
}

}  // namespace validatorsession

}  // namespace ton

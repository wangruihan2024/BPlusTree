#include "storage/index/b_plus_tree.h"

#include <sstream>
#include <string>

#include "buffer/lru_k_replacer.h"
#include "common/config.h"
#include "common/exception.h"
#include "common/logger.h"
#include "common/macros.h"
#include "common/rid.h"
#include "storage/index/index_iterator.h"
#include "storage/page/b_plus_tree_header_page.h"
#include "storage/page/b_plus_tree_internal_page.h"
#include "storage/page/b_plus_tree_leaf_page.h"
#include "storage/page/b_plus_tree_page.h"
#include "storage/page/page_guard.h"

namespace bustub
{

INDEX_TEMPLATE_ARGUMENTS
BPLUSTREE_TYPE::BPlusTree(std::string name, page_id_t header_page_id,
                          BufferPoolManager* buffer_pool_manager,
                          const KeyComparator& comparator, int leaf_max_size,
                          int internal_max_size)
    : index_name_(std::move(name)),
      bpm_(buffer_pool_manager),
      comparator_(std::move(comparator)),
      leaf_max_size_(leaf_max_size),
      internal_max_size_(internal_max_size),
      header_page_id_(header_page_id)
{
  {
  WritePageGuard guard = bpm_ -> FetchPageWrite(header_page_id_);
  // In the original bpt, I fetch the header page
  // thus there's at least one page now
  auto root_header_page = guard.template AsMut<BPlusTreeHeaderPage>();
  // reinterprete the data of the page into "HeaderPage"
  root_header_page -> root_page_id_ = INVALID_PAGE_ID;
  // set the root_id to INVALID
  }
  // bpm_->UnpinPage(header_page_id_, true);
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::PageCopy(WritePageGuard &be_copied, WritePageGuard &copied) {
  char *be_copied_ptr = reinterpret_cast<char *>(be_copied.AsMut<void>());
  char *copied_ptr = reinterpret_cast<char *>(copied.AsMut<void>());
  for (size_t i = 0; i < BUSTUB_PAGE_SIZE; ++i) {
    be_copied_ptr[i] =copied_ptr[i];
  }
}

/*
 * Helper function to decide whether current b+tree is empty
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::IsEmpty() const  ->  bool
{
  ReadPageGuard guard = bpm_ -> FetchPageRead(header_page_id_);
  auto root_header_page = guard.template As<BPlusTreeHeaderPage>();
  bool is_empty = root_header_page -> root_page_id_ == INVALID_PAGE_ID;
  // Just check if the root_page_id is INVALID
  // usage to fetch a page:
  // fetch the page guard   ->   call the "As" function of the page guard
  // to reinterprete the data of the page as "BPlusTreePage"
  return is_empty;
}
/*****************************************************************************
 * SEARCH
 *****************************************************************************/
/*
 * Return the only value that associated with input key
 * This method is used for point query
 * @return : true means key exists
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::GetValue(const KeyType& key,
                              std::vector<ValueType>* result, Transaction* txn)
     ->  bool
{
  //Your code here
  ReadPageGuard header_guard = bpm_->FetchPageRead(header_page_id_);
  auto root_header_page = header_guard.As<BPlusTreeHeaderPage>();
  bool is_empty = root_header_page->root_page_id_ == INVALID_PAGE_ID;
  if(is_empty)
    return false;
  header_guard = bpm_->FetchPageRead(root_header_page->root_page_id_);
  const InternalPage *tmp = header_guard.As<InternalPage>();
  while(!tmp->IsLeafPage()) {
    int new_tmp = BinaryFind(tmp, key);
    header_guard = std::move(bpm_->FetchPageRead(tmp->ValueAt(new_tmp)));
    tmp = header_guard.As<InternalPage>();
  }
  const LeafPage *leaf_page = header_guard.As<LeafPage>();
  int leaf_pos = BinaryFind(leaf_page, key);
  if(leaf_pos == -1 || comparator_(key, leaf_page->KeyAt(leaf_pos)) != 0)
    return false;
  result->push_back(leaf_page->ValueAt(leaf_pos));
  return true;
}
/*****************************************************************************
 * INSERTION
 *****************************************************************************/
/*
 * Insert constant key & value pair into b+ tree
 * if current tree is empty, start new tree, update root page id and insert
 * entry, otherwise insert into leaf page.
 * @return: since we only support unique key, if user try to insert duplicate
 * keys return false, otherwise return true.
 */

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Insert(const KeyType& key, const ValueType& value,
                            Transaction* txn) -> bool {
  WritePageGuard header_guard = bpm_->FetchPageWrite(header_page_id_);
  auto *header = header_guard.AsMut<BPlusTreeHeaderPage>();
  // 空树初始化
  if (header->root_page_id_ == INVALID_PAGE_ID) {
    page_id_t root_id;
    WritePageGuard root_guard = bpm_->NewPageGuarded(&root_id).UpgradeWrite();
    auto *root = root_guard.AsMut<LeafPage>();
    root->Init(leaf_max_size_);
    root->SetKeyAt(0, key);
    root->SetValueAt(0, value);
    root->IncreaseSize(1);
    header->root_page_id_ = root_id;
    return true;
  }
  std::deque<WritePageGuard> guards;
  std::deque<int> path;
  // 初始 root 节点
  guards.push_back(bpm_->FetchPageWrite(header->root_page_id_));
  path.push_back(-1);
  const auto *root_page = guards.front().As<BPlusTreePage>();
  bool root_not_full = root_page->GetSize() < root_page->GetMaxSize();
  if (root_not_full) {
    header_guard.Drop();
  }
  auto *node = guards.back().AsMut<InternalPage>();
  // 路径向下，直到叶子节点
  while (!node->IsLeafPage()) {
    int child_idx = BinaryFind(node, key);
    guards.push_back(bpm_->FetchPageWrite(node->ValueAt(child_idx)));
    path.push_back(child_idx);
    node = guards.back().AsMut<InternalPage>();
    if (!node->IsLeafPage() && node->GetSize() < node->GetMaxSize()) {
      if (!root_not_full) {
        header_guard.Drop();
        root_not_full = true;
      }
      while (guards.size() > 1) {
        guards.pop_front();
        path.pop_front();
      }
    }
  }
  auto &leaf_guard = guards.back();
  auto *leaf = leaf_guard.AsMut<LeafPage>();
  int pos = BinaryFind(leaf, key);
  if (pos != -1 && comparator_(leaf->KeyAt(pos), key) == 0) {
    return false;  // duplicate key
  }
  // 简单插入
  if (leaf->GetSize() < leaf->GetMaxSize()) {
    if (!root_not_full) {
      header_guard.Drop();
    }
    while (guards.size() > 1) {
      guards.pop_front();
      path.pop_front();
    }
    InsertAtLeaf(leaf, pos, key, value);
    return true;
  }
  // 分裂叶子节点
  KeyType mid_key;
  page_id_t left_id, right_id;
  std::tie(mid_key, left_id, right_id) = SplitAtLeaf(leaf_guard, pos, key, value);
  // 向上递归插入
  for (int i = static_cast<int>(guards.size()) - 2; i >= (root_not_full ? 1 : 0); i--) {
    auto &node_guard = guards[i];
    int child_idx = path[i + 1];
    std::tie(mid_key, left_id, right_id) = SplitAtInternal(node_guard, child_idx, mid_key, right_id);
  }
  if (root_not_full) {
    auto *root = guards.front().AsMut<InternalPage>();
    InsertAtInternal(root, path[1], mid_key, right_id);
  } else {
    header->root_page_id_ = PromoteRoot(mid_key, left_id, right_id);
  }
  return true;
}
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::InsertAtLeaf(LeafPage *node, int x, const KeyType &key, const ValueType &value) {
  node->IncreaseSize(1);
  for (int i = node->GetSize() - 1; i > x + 1; i--) {
    node->SetKeyAt(i, node->KeyAt(i - 1));
    node->SetValueAt(i, node->ValueAt(i - 1));
  }
  node->SetKeyAt(x + 1, key);
  node->SetValueAt(x + 1, value);
}
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::InsertAtInternal(InternalPage *node, int x, const KeyType &key, page_id_t value) {
  node->IncreaseSize(1);
  for (int i = node->GetSize() - 1; i > x + 1; i--) {
    node->SetKeyAt(i, node->KeyAt(i - 1));
    node->SetValueAt(i, node->ValueAt(i - 1));
  }
  node->SetKeyAt(x + 1, key);
  node->SetValueAt(x + 1, value);
}
INDEX_TEMPLATE_ARGUMENTS
 std::tuple<KeyType, page_id_t, page_id_t> BPLUSTREE_TYPE::SplitAtLeaf(WritePageGuard& node_guard, int x, const KeyType& key, const ValueType& value) {
  page_id_t node_id = node_guard.PageId();
  LeafPage* node = node_guard.AsMut<LeafPage>();
  page_id_t l_node_id, r_node_id; // l_node 只是临时缓存, 最后空间要还给 node
  WritePageGuard l_node_guard = bpm_->NewPageGuarded(&l_node_id).UpgradeWrite();
  WritePageGuard r_node_guard = bpm_->NewPageGuarded(&r_node_id).UpgradeWrite();
  LeafPage *l_node = l_node_guard.AsMut<LeafPage>(), *r_node = r_node_guard.AsMut<LeafPage>();
  l_node->Init(leaf_max_size_);
  r_node->Init(leaf_max_size_);
  for (int j = 0; j < l_node->GetMinSize(); j++) {
    l_node->IncreaseSize(1);
    if (j == x + 1) {
      l_node->SetKeyAt(j, key);
      l_node->SetValueAt(j, value);
    } else {
      l_node->SetKeyAt(j, 
        node->KeyAt(j - (j > x)));
      l_node->SetValueAt(j, 
        node->ValueAt(j - (j > x)));
    }
  }
  int l_size = l_node->GetSize();
  for (int j = 0; j < node->GetSize() + 1 - l_size; j++) {
    r_node->IncreaseSize(1);
    if (j + l_size == x + 1) {
      r_node->SetKeyAt(j, key);
      r_node->SetValueAt(j, value);
    } else {
      r_node->SetKeyAt(j,
        node->KeyAt(j + l_size - (j + l_size > x)));
      r_node->SetValueAt(j,
        node->ValueAt(j + l_size - (j + l_size > x)));
    }
  }

  l_node->SetNextPageId(r_node_id);
  r_node->SetNextPageId(node->GetNextPageId());
  bpm_->FlushPage(node_id);
  bpm_->FlushPage(r_node_id);
  PageCopy(node_guard, l_node_guard);

  return std::make_tuple(r_node->KeyAt(0), node_id, r_node_id);
}
INDEX_TEMPLATE_ARGUMENTS
 std::tuple<KeyType, page_id_t, page_id_t> BPLUSTREE_TYPE::SplitAtInternal(WritePageGuard& node_guard, int x, const KeyType& key, page_id_t value /* child page_id */) {
  page_id_t node_id = node_guard.PageId();
  InternalPage* node = node_guard.AsMut<InternalPage>();
  page_id_t l_node_id, r_node_id;
  WritePageGuard l_node_guard = bpm_->NewPageGuarded(&l_node_id).UpgradeWrite();
  WritePageGuard r_node_guard = bpm_->NewPageGuarded(&r_node_id).UpgradeWrite();
  InternalPage *l_node = l_node_guard.AsMut<InternalPage>(), *r_node = r_node_guard.AsMut<InternalPage>();
  l_node->Init(internal_max_size_), r_node->Init(internal_max_size_);
  l_node->SetSize(0), r_node->SetSize(0); 

  for (int j = 0; j < l_node->GetMinSize(); j++) {
    l_node->IncreaseSize(1);
    if (j == x + 1) {
      l_node->SetKeyAt(j, key);
      l_node->SetValueAt(j, value);
    } else {
      l_node->SetKeyAt(j, 
        node->KeyAt(j - (j > x)));
      l_node->SetValueAt(j, 
        node->ValueAt(j - (j > x)));
    }
  }
  int l_size = l_node->GetSize();
  for (int j = 0; j < node->GetSize() + 1 - l_size; j++) {
    r_node->IncreaseSize(1);
    if (j + l_size == x + 1) {
      r_node->SetKeyAt(j, key);
      r_node->SetValueAt(j, value);
    } else {
      r_node->SetKeyAt(j,
        node->KeyAt(j + l_size - (j + l_size > x)));
      r_node->SetValueAt(j,
        node->ValueAt(j + l_size - (j + l_size > x)));
    }
  }
  PageCopy(node_guard, l_node_guard);
  bpm_->FlushPage(node_id);
  bpm_->FlushPage(r_node_id);
  return std::make_tuple(r_node->KeyAt(0), node_id, r_node_id); // 从代码来看，0 处是有内存空间的。
}
INDEX_TEMPLATE_ARGUMENTS
page_id_t BPLUSTREE_TYPE::PromoteRoot(const KeyType& key, page_id_t l_child, page_id_t r_child) {
  page_id_t root_id;
  WritePageGuard root_guard = bpm_->NewPageGuarded(&root_id).UpgradeWrite();
  InternalPage *root = root_guard.AsMut<InternalPage>();
  root->Init(internal_max_size_);
  root->SetSize(2);
  root->SetKeyAt(1, key);
  root->SetValueAt(0, l_child);
  root->SetValueAt(1, r_child);
  bpm_->FlushPage(root_id);
  return root_id;
}



/*****************************************************************************
 * REMOVE
 *****************************************************************************/
/*
 * Delete key & value pair associated with input key
 * If current tree is empty, return immediately.
 * If not, User needs to first find the right leaf page as deletion target, then
 * delete entry from leaf page. Remember to deal with redistribute or merge if
 * necessary.
 */

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::Remove(const KeyType& key, Transaction* txn)
{
  WritePageGuard header_guard = bpm_->FetchPageWrite(header_page_id_);
  BPlusTreeHeaderPage *header = header_guard.AsMut<BPlusTreeHeaderPage>();
  if (header->root_page_id_ == INVALID_PAGE_ID) 
    return;
  std::deque<WritePageGuard> guards;
  guards.push_back(std::move(bpm_ -> FetchPageWrite(header->root_page_id_)));
  const BPlusTreePage* root_general = guards.front().As<BPlusTreePage>();
  bool root_released = (root_general->GetSize() != 2); // whether root is full and will split after insert
  if (root_released)
    header_guard.Drop();
  std::deque<int> target_childs(1, -1);
  InternalPage* node = guards.back().AsMut<InternalPage>();
  while (!node->IsLeafPage()) {
    int target_child = BinaryFind(node, key);
    target_childs.push_back(target_child);
    guards.push_back(std::move(
      bpm_ -> FetchPageWrite(node->ValueAt(target_child))
    ));
    node = guards.back().AsMut<InternalPage>();
    if (!node->IsLeafPage() && node->GetSize() > node->GetMinSize())
      while (guards.size() > 1) {
        target_childs.pop_front();
        guards.pop_front();
        if (!root_released) {
          header_guard.Drop();
          root_released = true;
        }
      }
  }
  WritePageGuard& leaf_guard = guards.back();
  LeafPage* leaf = leaf_guard.AsMut<LeafPage>();
  int target_position = BinaryFind(leaf, key);
  if (target_position == -1 || comparator_(leaf->KeyAt(target_position), key) != 0) 
    return;
  EraseAtLeaf(leaf, target_position); 
  if (leaf->GetSize() >= leaf->GetMinSize() || root_general->IsLeafPage()) { 
    while (guards.size() > 1)
      guards.pop_front();
    if (root_general->IsLeafPage() && leaf->GetSize() == 0) 
      header->root_page_id_ = INVALID_PAGE_ID;
  } else { 
    page_id_t new_node = INVALID_PAGE_ID;
    InternalPage* parent = guards[guards.size() - 2].AsMut<InternalPage>();
    int leaf_pos = target_childs.back();
    int sibling_dir = leaf_pos != parent->GetSize() - 1 ? 1 : -1;
    int sibling_pos = leaf_pos + sibling_dir;
    WritePageGuard sibling_guard = bpm_ -> FetchPageWrite(parent->ValueAt(sibling_pos));
    LeafPage* sibling = sibling_guard.AsMut<LeafPage>();
    if (sibling->GetSize() > sibling->GetMinSize()) {
      if (sibling_dir > 0)
        BorrowAtLeaf(leaf, sibling, 1, parent, sibling_pos);
      else 
        BorrowAtLeaf(sibling, leaf, -1, parent, leaf_pos);
      if (!root_released) {
        header_guard.Drop();
        root_released = true; 
      }
    } else {
      if (sibling_dir > 0) 
        new_node = MergeAtLeaf(leaf_guard, sibling_guard, parent, leaf_pos);
      else 
        new_node = MergeAtLeaf(sibling_guard, leaf_guard, parent, sibling_pos);

      for (int i = guards.size() - 2; i >= 1; i--) {
        InternalPage* parent = guards[i - 1].AsMut<InternalPage>();
        WritePageGuard& node_guard = guards[i];
        InternalPage* node = node_guard.AsMut<InternalPage>();
        int node_pos = target_childs[i]; 
        int sibling_dir = node_pos != parent->GetSize() - 1 ? 1 : -1; 
        int sibling_pos = node_pos + sibling_dir;
        WritePageGuard sibling_guard = bpm_->FetchPageWrite(parent->ValueAt(sibling_pos));
        InternalPage* sibling = sibling_guard.AsMut<InternalPage>();
        if (sibling->GetSize() > sibling->GetMinSize()) {
          if (sibling_dir > 0)
            BorrowAtInternal(node, sibling, 1, parent, sibling_pos);
          else 
            BorrowAtInternal(sibling, node, -1, parent, node_pos);
          if (!root_released) {
            header_guard.Drop();
            root_released = true; 
          }
          break;
        } else {
          if (sibling_dir > 0)
            new_node = MergeAtInternal(node_guard, sibling_guard, parent, node_pos);
          else
            new_node = MergeAtInternal(sibling_guard, node_guard, parent, sibling_pos);
        }
      }
      if (!root_released)
          header->root_page_id_ = new_node;
    }
  }
}


INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::EraseAtLeaf(LeafPage *node, int x) {
  for (int i = x; i < node->GetSize() - 1; i++) {
    node->SetKeyAt(i, node->KeyAt(i + 1));
    node->SetValueAt(i, node->ValueAt(i + 1));
  }
  node->IncreaseSize(-1);
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::EraseAtInternal(InternalPage *node, int x) {
  for (int i = x; i < node->GetSize() - 1; i++) {
    node->SetKeyAt(i, node->KeyAt(i + 1));
    node->SetValueAt(i, node->ValueAt(i + 1));
  }
  node->IncreaseSize(-1);
}
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::BorrowAtLeaf(LeafPage* l_node, LeafPage* r_node, int sibling_dir, InternalPage *parent, int y) {
  if (sibling_dir == 1) {
    InsertAtLeaf(l_node, l_node->GetSize() - 1, r_node->KeyAt(0), r_node->ValueAt(0));
    EraseAtLeaf(r_node, 0);
    parent->SetKeyAt(y, r_node->KeyAt(0));
  } else {
    InsertAtLeaf(r_node, -1, l_node->KeyAt(l_node->GetSize() - 1), l_node->ValueAt(l_node->GetSize() - 1)); 
    EraseAtLeaf(l_node, l_node->GetSize() - 1);
    parent->SetKeyAt(y, r_node->KeyAt(0));
  }
}
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::BorrowAtInternal(InternalPage *l_node, InternalPage *r_node, int sibling_dir, InternalPage *parent, int y ) {
  if (sibling_dir == 1) { 
    InsertAtInternal(l_node, l_node->GetSize() - 1, r_node->KeyAt(0), r_node->ValueAt(0));
    EraseAtInternal(r_node, 0);
    parent->SetKeyAt(y, r_node->KeyAt(0));
  } else {
    InsertAtInternal(r_node, -1, l_node->KeyAt(l_node->GetSize() - 1), l_node->ValueAt(l_node->GetSize() - 1)); 
    EraseAtInternal(l_node, l_node->GetSize() - 1);
    parent->SetKeyAt(y, r_node->KeyAt(0));
  }
}
INDEX_TEMPLATE_ARGUMENTS
page_id_t BPLUSTREE_TYPE::MergeAtLeaf(WritePageGuard& l_node_guard, WritePageGuard& r_node_guard, InternalPage *parent, int x) {
  page_id_t l_node_id = l_node_guard.PageId();
  LeafPage* l_node = l_node_guard.AsMut<LeafPage>(), *r_node = r_node_guard.AsMut<LeafPage>();
  page_id_t node_id;
  WritePageGuard node_guard = bpm_ -> NewPageGuarded(&node_id).UpgradeWrite();
  LeafPage *node = node_guard.AsMut<LeafPage>();
  node->Init(leaf_max_size_);
  node->SetSize(l_node->GetSize() + r_node->GetSize());
  for (int j = 0; j < l_node->GetSize(); j++) {
    node->SetKeyAt(j, l_node->KeyAt(j));
    node->SetValueAt(j, l_node->ValueAt(j));
  }
  int l_size = l_node->GetSize();
  for (int j = 0; j < r_node->GetSize(); j++) {
    node->SetKeyAt(j + l_size, r_node->KeyAt(j));
    node->SetValueAt(j + l_size, r_node->ValueAt(j));
  }
  EraseAtInternal(parent, x + 1);
  parent->SetKeyAt(x, node->KeyAt(0));
  node->SetNextPageId(r_node->GetNextPageId());
  PageCopy(l_node_guard, node_guard);
  return l_node_id;
}
INDEX_TEMPLATE_ARGUMENTS
page_id_t BPLUSTREE_TYPE::MergeAtInternal(WritePageGuard& l_node_guard, WritePageGuard& r_node_guard, InternalPage *parent, int x) {
  page_id_t l_node_id = l_node_guard.PageId();
  InternalPage* l_node = l_node_guard.AsMut<InternalPage>(), *r_node = r_node_guard.AsMut<InternalPage>();
  page_id_t node_id;
  WritePageGuard node_guard = bpm_ -> NewPageGuarded(&node_id).UpgradeWrite();
  InternalPage *node = node_guard.AsMut<InternalPage>();
  node->Init(internal_max_size_);
node->SetSize(l_node->GetSize() + r_node->GetSize());
  for (int j = 0; j < l_node->GetSize(); j++) {
    node->SetKeyAt(j, l_node->KeyAt(j));
    node->SetValueAt(j, l_node->ValueAt(j));
  }
  int l_size = l_node->GetSize();
  for (int j = 0; j < r_node->GetSize(); j++) {
    node->SetKeyAt(j + l_size, r_node->KeyAt(j));
    node->SetValueAt(j + l_size, r_node->ValueAt(j));
  }
  EraseAtInternal(parent, x + 1);
  parent->SetKeyAt(x, node->KeyAt(0));
  PageCopy(l_node_guard, node_guard);
  return l_node_id;
}
/*****************************************************************************
 * INDEX ITERATOR
 *****************************************************************************/


INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::BinaryFind(const LeafPage* leaf_page, const KeyType& key)
     ->  int
{
  int l = 0;
  int r = leaf_page -> GetSize() - 1;
  while (l < r)
  {
    int mid = (l + r + 1) >> 1;
    if (comparator_(leaf_page -> KeyAt(mid), key) != 1)
    {
      l = mid;
    }
    else 
    {
      r = mid - 1;
    }
  }

  if (r >= 0 && comparator_(leaf_page -> KeyAt(r), key) == 1)
  {
    r = -1;
  }

  return r;
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::BinaryFind(const InternalPage* internal_page,
                                const KeyType& key)  ->  int
{
  int l = 1;
  int r = internal_page -> GetSize() - 1;
  while (l < r)
  {
    int mid = (l + r + 1) >> 1;
    if (comparator_(internal_page -> KeyAt(mid), key) != 1) 
    {
      l = mid;
    }
    else
    {
      r = mid - 1;
    }
  }

  if (r == -1 || comparator_(internal_page -> KeyAt(r), key) == 1)
  {
    r = 0;
  }

  return r;
}

/*
 * Input parameter is void, find the leftmost leaf page first, then construct
 * index iterator
 * @return : index iterator
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Begin()  ->  INDEXITERATOR_TYPE
//Just go left forever
{
  ReadPageGuard head_guard = bpm_ -> FetchPageRead(header_page_id_);
  if (head_guard.template As<BPlusTreeHeaderPage>() -> root_page_id_ == INVALID_PAGE_ID)
  {
    return End();
  }
  ReadPageGuard guard = bpm_ -> FetchPageRead(head_guard.As<BPlusTreeHeaderPage>() -> root_page_id_);
  head_guard.Drop();

  auto tmp_page = guard.template As<BPlusTreePage>();
  while (!tmp_page -> IsLeafPage())
  {
    int slot_num = 0;
    guard = bpm_ -> FetchPageRead(reinterpret_cast<const InternalPage*>(tmp_page) -> ValueAt(slot_num));
    tmp_page = guard.template As<BPlusTreePage>();
  }
  int slot_num = 0;
  if (slot_num != -1)
  {
    return INDEXITERATOR_TYPE(bpm_, guard.PageId(), 0);
  }
  return End();
}


/*
 * Input parameter is low key, find the leaf page that contains the input key
 * first, then construct index iterator
 * @return : index iterator
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Begin(const KeyType& key)  ->  INDEXITERATOR_TYPE
{
  ReadPageGuard head_guard = bpm_ -> FetchPageRead(header_page_id_);

  if (head_guard.template As<BPlusTreeHeaderPage>() -> root_page_id_ == INVALID_PAGE_ID)
  {
    return End();
  }
  ReadPageGuard guard = bpm_ -> FetchPageRead(head_guard.As<BPlusTreeHeaderPage>() -> root_page_id_);
  head_guard.Drop();
  auto tmp_page = guard.template As<BPlusTreePage>();
  while (!tmp_page -> IsLeafPage())
  {
    auto internal = reinterpret_cast<const InternalPage*>(tmp_page);
    int slot_num = BinaryFind(internal, key);
    if (slot_num == -1)
    {
      return End();
    }
    guard = bpm_ -> FetchPageRead(reinterpret_cast<const InternalPage*>(tmp_page) -> ValueAt(slot_num));
    tmp_page = guard.template As<BPlusTreePage>();
  }
  auto* leaf_page = reinterpret_cast<const LeafPage*>(tmp_page);

  int slot_num = BinaryFind(leaf_page, key);
  if (slot_num != -1)
  {
    return INDEXITERATOR_TYPE(bpm_, guard.PageId(), slot_num);
  }
  return End();
}

/*
 * Input parameter is void, construct an index iterator representing the end
 * of the key/value pair in the leaf node
 * @return : index iterator
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::End()  ->  INDEXITERATOR_TYPE
{
  return INDEXITERATOR_TYPE(bpm_, -1, -1);
}

/**
 * @return Page id of the root of this tree
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::GetRootPageId()  ->  page_id_t
{
  // std::cout << "stuck pos 0" << std::endl;
  ReadPageGuard guard = bpm_ -> FetchPageRead(header_page_id_);
  // std::cout << "stuck pos 1" << std::endl;
  auto root_header_page = guard.template As<BPlusTreeHeaderPage>();
  // std::cout << "stuck pos 2" << std::endl;
  page_id_t root_page_id = root_header_page -> root_page_id_;
  // std::cout << "stuck pos 3" << std::endl;
  return root_page_id;
}

/*****************************************************************************
 * UTILITIES AND DEBUG
 *****************************************************************************/

/*
 * This method is used for test only
 * Read data from file and insert one by one
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::InsertFromFile(const std::string& file_name,
                                    Transaction* txn)
{
  int64_t key;
  std::ifstream input(file_name);
  while (input >> key)
  {
    KeyType index_key;
    index_key.SetFromInteger(key);
    RID rid(key);
    Insert(index_key, rid, txn);
  }
}
/*
 * This method is used for test only
 * Read data from file and remove one by one
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::RemoveFromFile(const std::string& file_name,
                                    Transaction* txn)
{
  int64_t key;
  std::ifstream input(file_name);
  while (input >> key)
  {
    KeyType index_key;
    index_key.SetFromInteger(key);
    Remove(index_key, txn);
  }
}

/*
 * This method is used for test only
 * Read data from file and insert/remove one by one
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::BatchOpsFromFile(const std::string& file_name,
                                      Transaction* txn)
{
  int64_t key;
  char instruction;
  std::ifstream input(file_name);
  while (input)
  {
    input >> instruction >> key;
    RID rid(key);
    KeyType index_key;
    index_key.SetFromInteger(key);
    switch (instruction)
    {
      case 'i':
        Insert(index_key, rid, txn);
        break;
      case 'd':
        Remove(index_key, txn);
        break;
      default:
        break;
    }
  }
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::Print(BufferPoolManager* bpm)
{
  auto root_page_id = GetRootPageId();
  auto guard = bpm -> FetchPageBasic(root_page_id);
  PrintTree(guard.PageId(), guard.template As<BPlusTreePage>());
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::PrintTree(page_id_t page_id, const BPlusTreePage* page)
{
  if (page -> IsLeafPage())
  {
    auto* leaf = reinterpret_cast<const LeafPage*>(page);
    std::cout << "Leaf Page: " << page_id << "\tNext: " << leaf -> GetNextPageId() << std::endl;

    // Print the contents of the leaf page.
    std::cout << "Contents: ";
    for (int i = 0; i < leaf -> GetSize(); i++)
    {
      std::cout << leaf -> KeyAt(i);
      if ((i + 1) < leaf -> GetSize())
      {
        std::cout << ", ";
      }
    }
    std::cout << std::endl;
    std::cout << std::endl;
  }
  else
  {
    auto* internal = reinterpret_cast<const InternalPage*>(page);
    std::cout << "Internal Page: " << page_id << std::endl;

    // Print the contents of the internal page.
    std::cout << "Contents: ";
    for (int i = 0; i < internal -> GetSize(); i++)
    {
      std::cout << internal -> KeyAt(i) << ": " << internal -> ValueAt(i);
      if ((i + 1) < internal -> GetSize())
      {
        std::cout << ", ";
      }
    }
    std::cout << std::endl;
    std::cout << std::endl;
    for (int i = 0; i < internal -> GetSize(); i++)
    {
      auto guard = bpm_ -> FetchPageBasic(internal -> ValueAt(i));
      PrintTree(guard.PageId(), guard.template As<BPlusTreePage>());
    }
  }
}

/**
 * This method is used for debug only, You don't need to modify
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::Draw(BufferPoolManager* bpm, const std::string& outf)
{
  if (IsEmpty())
  {
    LOG_WARN("Drawing an empty tree");
    return;
  }

  std::ofstream out(outf);
  out << "digraph G {" << std::endl;
  auto root_page_id = GetRootPageId();
  auto guard = bpm -> FetchPageBasic(root_page_id);
  ToGraph(guard.PageId(), guard.template As<BPlusTreePage>(), out);
  out << "}" << std::endl;
  out.close();
}

/**
 * This method is used for debug only, You don't need to modify
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::ToGraph(page_id_t page_id, const BPlusTreePage* page,
                             std::ofstream& out)
{
  std::string leaf_prefix("LEAF_");
  std::string internal_prefix("INT_");
  if (page -> IsLeafPage())
  {
    auto* leaf = reinterpret_cast<const LeafPage*>(page);
    // Print node name
    out << leaf_prefix << page_id;
    // Print node properties
    out << "[shape=plain color=green ";
    // Print data of the node
    out << "label=<<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" "
           "CELLPADDING=\"4\">\n";
    // Print data
    out << "<TR><TD COLSPAN=\"" << leaf -> GetSize() << "\">P=" << page_id
        << "</TD></TR>\n";
    out << "<TR><TD COLSPAN=\"" << leaf -> GetSize() << "\">"
        << "max_size=" << leaf -> GetMaxSize()
        << ",min_size=" << leaf -> GetMinSize() << ",size=" << leaf -> GetSize()
        << "</TD></TR>\n";
    out << "<TR>";
    for (int i = 0; i < leaf -> GetSize(); i++)
    {
      out << "<TD>" << leaf -> KeyAt(i) << "</TD>\n";
    }
    out << "</TR>";
    // Print table end
    out << "</TABLE>>];\n";
    // Print Leaf node link if there is a next page
    if (leaf -> GetNextPageId() != INVALID_PAGE_ID)
    {
      out << leaf_prefix << page_id << "   ->   " << leaf_prefix
          << leaf -> GetNextPageId() << ";\n";
      out << "{rank=same " << leaf_prefix << page_id << " " << leaf_prefix
          << leaf -> GetNextPageId() << "};\n";
    }
  }
  else
  {
    auto* inner = reinterpret_cast<const InternalPage*>(page);
    // Print node name
    out << internal_prefix << page_id;
    // Print node properties
    out << "[shape=plain color=pink ";  // why not?
    // Print data of the node
    out << "label=<<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" "
           "CELLPADDING=\"4\">\n";
    // Print data
    out << "<TR><TD COLSPAN=\"" << inner -> GetSize() << "\">P=" << page_id
        << "</TD></TR>\n";
    out << "<TR><TD COLSPAN=\"" << inner -> GetSize() << "\">"
        << "max_size=" << inner -> GetMaxSize()
        << ",min_size=" << inner -> GetMinSize() << ",size=" << inner -> GetSize()
        << "</TD></TR>\n";
    out << "<TR>";
    for (int i = 0; i < inner -> GetSize(); i++)
    {
      out << "<TD PORT=\"p" << inner -> ValueAt(i) << "\">";
      // if (i > 0) {
      out << inner -> KeyAt(i) << "  " << inner -> ValueAt(i);
      // } else {
      // out << inner  ->  ValueAt(0);
      // }
      out << "</TD>\n";
    }
    out << "</TR>";
    // Print table end
    out << "</TABLE>>];\n";
    // Print leaves
    for (int i = 0; i < inner -> GetSize(); i++)
    {
      auto child_guard = bpm_ -> FetchPageBasic(inner -> ValueAt(i));
      auto child_page = child_guard.template As<BPlusTreePage>();
      ToGraph(child_guard.PageId(), child_page, out);
      if (i > 0)
      {
        auto sibling_guard = bpm_ -> FetchPageBasic(inner -> ValueAt(i - 1));
        auto sibling_page = sibling_guard.template As<BPlusTreePage>();
        if (!sibling_page -> IsLeafPage() && !child_page -> IsLeafPage())
        {
          out << "{rank=same " << internal_prefix << sibling_guard.PageId()
              << " " << internal_prefix << child_guard.PageId() << "};\n";
        }
      }
      out << internal_prefix << page_id << ":p" << child_guard.PageId()
          << "   ->   ";
      if (child_page -> IsLeafPage())
      {
        out << leaf_prefix << child_guard.PageId() << ";\n";
      }
      else
      {
        out << internal_prefix << child_guard.PageId() << ";\n";
      }
    }
  }
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::DrawBPlusTree()  ->  std::string
{
  if (IsEmpty())
  {
    return "()";
  }

  PrintableBPlusTree p_root = ToPrintableBPlusTree(GetRootPageId());
  std::ostringstream out_buf;
  p_root.Print(out_buf);

  return out_buf.str();
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::ToPrintableBPlusTree(page_id_t root_id)
     ->  PrintableBPlusTree
{
  auto root_page_guard = bpm_ -> FetchPageBasic(root_id);
  auto root_page = root_page_guard.template As<BPlusTreePage>();
  PrintableBPlusTree proot;

  if (root_page -> IsLeafPage())
  {
    auto leaf_page = root_page_guard.template As<LeafPage>();
    proot.keys_ = leaf_page -> ToString();
    proot.size_ = proot.keys_.size() + 4;  // 4 more spaces for indent

    return proot;
  }

  // draw internal page
  auto internal_page = root_page_guard.template As<InternalPage>();
  proot.keys_ = internal_page -> ToString();
  proot.size_ = 0;
  for (int i = 0; i < internal_page -> GetSize(); i++)
  {
    page_id_t child_id = internal_page -> ValueAt(i);
    PrintableBPlusTree child_node = ToPrintableBPlusTree(child_id);
    proot.size_ += child_node.size_;
    proot.children_.push_back(child_node);
  }

  return proot;
}

template class BPlusTree<GenericKey<4>, RID, GenericComparator<4>>;

template class BPlusTree<GenericKey<8>, RID, GenericComparator<8>>;

template class BPlusTree<GenericKey<16>, RID, GenericComparator<16>>;

template class BPlusTree<GenericKey<32>, RID, GenericComparator<32>>;

template class BPlusTree<GenericKey<64>, RID, GenericComparator<64>>;

}  // namespace bustub
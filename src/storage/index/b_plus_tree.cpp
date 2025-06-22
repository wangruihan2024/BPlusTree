#include "storage/index/b_plus_tree.h"

#include <sstream>
#include <string>
#include <queue>
#include <vector>
#include <queue>

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

int insert_cnt = 0;
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Insert(const KeyType& key, const ValueType& value,
                            Transaction* txn)  ->  bool
{
  insert_cnt++;
  // std::cout << "this is the " << insert_cnt << " th insert" << std::endl; 
  // std::cout << DrawBPlusTree() << std::endl;
  // std::cout << ">>> Insert started" << std::endl;
  //Your code here
  WritePageGuard header_guard = bpm_->FetchPageWrite(header_page_id_);
  BPlusTreeHeaderPage* root_header_page = header_guard.AsMut<BPlusTreeHeaderPage>();
  // empty
  bool is_empty = root_header_page->root_page_id_ == INVALID_PAGE_ID;
  if(is_empty) {
    WritePageGuard new_root = (bpm_->NewPageGuarded(&root_header_page->root_page_id_)).UpgradeWrite();
    LeafPage *root = new_root.AsMut<LeafPage>();
    root->Init(leaf_max_size_);
    root->IncreaseSize(1);
    root->SetKeyAt(0, key);
    root->SetValueAt(0, value);

    return true;
  }
  // 不能用queue不支持随机访问
  // find node
  std::deque<WritePageGuard> guards; // guards[i] the lock on node on the ith level
  std::deque<int> childs;            // guards[i] is the childs[i]th child of guards[i - 1]
  childs.push_back(-1);
  guards.push_back(bpm_->FetchPageWrite(root_header_page->root_page_id_));
  const BPlusTreePage *root = guards.front().As<BPlusTreePage>();
  bool release_root = root->GetSize() < root->GetMaxSize();
  if(release_root)
    header_guard.Drop();
  InternalPage *tmp = guards.back().AsMut<InternalPage>();
  while(!tmp->IsLeafPage()) {
    int new_tmp = BinaryFind(tmp, key);
    guards.push_back(bpm_->FetchPageWrite(tmp->ValueAt(new_tmp)));
    childs.push_back(new_tmp);
    tmp = guards.back().AsMut<InternalPage>();
    if(!tmp->IsLeafPage() && tmp->GetMaxSize() > tmp->GetSize()) {
      if(!release_root) {          // header haven't been released 
        header_guard.Drop();
        release_root = true;
      }
      while(guards.size() > 1) {
        guards.pop_front();
        childs.pop_front();
      }
    }
  }
  WritePageGuard& leaf_target = guards.back();
  LeafPage* leaf_page = leaf_target.AsMut<LeafPage>();
  int leaf_pos = BinaryFind(leaf_page, key);
  if(leaf_pos != -1 && comparator_(key, leaf_page->KeyAt(leaf_pos)) == 0)
    return false;
  // with no split
  // std::cout << "the leaf page has a size of " << leaf_page->GetSize() << std::endl;
  if(leaf_page->GetSize() < leaf_page->GetMaxSize()) {
    if(!release_root) {
      header_guard.Drop();
      release_root = true;
    }
    while(guards.size() > 1) {
      guards.pop_front();
      // childs.pop_front();
    }
    leaf_page->IncreaseSize(1);
    for(int i = leaf_page->GetSize() - 1; i > leaf_pos + 1; i--) {
      leaf_page->SetKeyAt(i, leaf_page->KeyAt(i - 1));
      leaf_page->SetValueAt(i, leaf_page->ValueAt(i - 1));
    }
    leaf_page->SetKeyAt(leaf_pos + 1, key);
    leaf_page->SetValueAt(leaf_pos + 1, value);
    return true;
  }
  // with split
  // page_id_t insert_idx = childs.top();  
    // split leaf
  // std::cout << "the case do need to split leaf" << std::endl;
  KeyType split_key;
  page_id_t left, right;
  {
    page_id_t leaf_id = leaf_target.PageId();
    page_id_t left_new_id, right_new_id;
    WritePageGuard left_new_guard = bpm_->NewPageGuarded(&left_new_id).UpgradeWrite();
    WritePageGuard right_new_guard = bpm_->NewPageGuarded(&right_new_id).UpgradeWrite();
    LeafPage *left_new_page = left_new_guard.AsMut<LeafPage>();
    LeafPage *right_new_page = right_new_guard.AsMut<LeafPage>();
    left_new_page->Init(leaf_max_size_);
    right_new_page->Init(leaf_max_size_);
    bool if_insert = false;
    for(int i = 0; i < left_new_page->GetMinSize(); i++) {
      left_new_page->IncreaseSize(1);
      if(i == leaf_pos + 1) {
        // std::cout << "insert at the new-left page with position:" << i << std::endl;
        left_new_page->SetKeyAt(i, key);
        left_new_page->SetValueAt(i, value);
        if_insert = true;
      } else {
        if(if_insert) {
          left_new_page->SetKeyAt(i, leaf_page->KeyAt(i - 1));
          left_new_page->SetValueAt(i, leaf_page->ValueAt(i - 1));
        } else {
          left_new_page->SetKeyAt(i, leaf_page->KeyAt(i));
          left_new_page->SetValueAt(i, leaf_page->ValueAt(i));
        }
      }
    }
    // std::cout << "the target leaf pos is" << leaf_pos << std::endl;
    // std::cout << "the size of the new left page is " << left_new_page->GetSize() << std::endl;
    for(int i = 0; i + left_new_page->GetSize() < leaf_page->GetSize() + 1; i++) {
      right_new_page->IncreaseSize(1);
      if((i + left_new_page->GetSize()) == (leaf_pos + 1)) {
        // std::cout << "insert at the new-right page with position:" << i << std::endl;
        right_new_page->SetKeyAt(i, key);
        right_new_page->SetValueAt(i, value);
        if_insert = true;
      }else {
        if(if_insert) {
          right_new_page->SetKeyAt(i, leaf_page->KeyAt(i + left_new_page->GetSize() - 1));
          right_new_page->SetValueAt(i, leaf_page->ValueAt(i + left_new_page->GetSize() - 1));
        }else {
          right_new_page->SetKeyAt(i, leaf_page->KeyAt(i + left_new_page->GetSize()));
          right_new_page->SetValueAt(i, leaf_page->ValueAt(i + left_new_page->GetSize()));
        }
      }
    }
    right_new_page->SetNextPageId(leaf_page->GetNextPageId());
    leaf_page->SetNextPageId(right_new_id);
    PageCopy(leaf_target, left_new_guard); 
    split_key = right_new_page->KeyAt(0);
    left = leaf_id;
    right = right_new_id;
    bpm_->FlushPage(left);
    bpm_->FlushPage(right);
  }
    // simple split to the last necessary floor
  for(int i = guards.size() - 2; i >= release_root; i--) {
    // std::cout << "the case do need to split the internal" << std::endl;
    WritePageGuard &tmp_guard = guards[i]; // node_guard, target_childs[i + 1], split_key, r_chi
    {
      page_id_t tmp_id = tmp_guard.PageId();
      InternalPage *tmp_page = tmp_guard.AsMut<InternalPage>();
      page_id_t tmp_left_id, tmp_right_id;
      WritePageGuard tmp_left_guard = bpm_->NewPageGuarded(&tmp_left_id).UpgradeWrite();
      WritePageGuard tmp_right_guard = bpm_->NewPageGuarded(&tmp_right_id).UpgradeWrite();
      InternalPage *tmp_left_page = tmp_left_guard.AsMut<InternalPage>();
      InternalPage *tmp_right_page = tmp_right_guard.AsMut<InternalPage>();
      tmp_left_page->Init(internal_max_size_);
      tmp_right_page->Init(internal_max_size_);
      tmp_left_page->SetSize(0);
      tmp_right_page->SetSize(0);
      // 这里需要把size初始化为0， init是初始化为1！！！
      bool if_inserted = false;
      for(int j = 0; j < tmp_left_page->GetMinSize(); j++) {
        tmp_left_page->IncreaseSize(1);
        if(j == childs[i + 1] + 1) {
          tmp_left_page->SetKeyAt(j, split_key);
          tmp_left_page->SetValueAt(j, right);
          if_inserted = true;
        } else {
          if(if_inserted) {
            tmp_left_page->SetKeyAt(j, tmp_page->KeyAt(j - 1));
            tmp_left_page->SetValueAt(j, tmp_page->ValueAt(j - 1));
          } else {
            tmp_left_page->SetKeyAt(j, tmp_page->KeyAt(j));
            tmp_left_page->SetValueAt(j, tmp_page->ValueAt(j));
          }
        }
      }
      // size_t left_size = tmp_left_page->GetSize();
      for(int j = 0; j + tmp_left_page->GetSize() < tmp_page->GetSize() + 1; j++) {
        tmp_right_page->IncreaseSize(1);
        if(j + tmp_left_page->GetSize() == childs[i + 1] + 1) {
          tmp_right_page->SetKeyAt(j, split_key);
          tmp_right_page->SetValueAt(j, right);
          if_inserted = true;
        } else {
          if(if_inserted) {
            tmp_right_page->SetKeyAt(j, tmp_page->KeyAt(j + tmp_left_page->GetSize() - 1));
            tmp_right_page->SetValueAt(j, tmp_page->ValueAt(j + tmp_left_page->GetSize() - 1));
          } else {
            tmp_right_page->SetKeyAt(j, tmp_page->KeyAt(j + tmp_left_page->GetSize()));
            tmp_right_page->SetValueAt(j, tmp_page->ValueAt(j + tmp_left_page->GetSize()));
          }
        }
      }
      PageCopy(tmp_guard, tmp_left_guard);
      split_key = tmp_right_page->KeyAt(0);
      left = tmp_id;
      right = tmp_right_id;
      bpm_->FlushPage(left);
      bpm_->FlushPage(right);
    }
  }
    // split without root
  if(release_root) {
    InternalPage *tmp = guards.front().AsMut<InternalPage>();
    // std::cout << "tmp has a last size of " << tmp->GetSize() << std::endl;
    tmp->IncreaseSize(1);
    for(int i = tmp->GetSize() - 1; i > childs[1] + 1; i--) {
      tmp->SetKeyAt(i, tmp->KeyAt(i - 1));
      tmp->SetValueAt(i, tmp->ValueAt(i - 1));
    }
    tmp->SetKeyAt(childs[1] + 1, split_key);
    tmp->SetValueAt(childs[1] + 1, right);
    return true;
  } else {
    // std::cout << "the root need to split" << std::endl;
    // 不能不写else 前一步改right
    // split with root
    page_id_t new_root_id;
    WritePageGuard new_root_guard = bpm_->NewPageGuarded(&new_root_id).UpgradeWrite();
    InternalPage *new_root_page = new_root_guard.AsMut<InternalPage>();
    new_root_page->Init(internal_max_size_);
    new_root_page->SetSize(2);
    new_root_page->SetValueAt(0, left);
    new_root_page->SetValueAt(1, right);
    new_root_page->SetKeyAt(1, split_key);
    root_header_page->root_page_id_ = new_root_id;
    bpm_->FlushPage(root_header_page->root_page_id_);
    return true;
  }
  guards.clear();
  return true;
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
  //Your code here
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
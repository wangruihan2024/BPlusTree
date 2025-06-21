//Your code here
  WritePageGuard header_guard = bpm_->FetchPageWrite(header_page_id_);
  auto root_header_page = header_guard.AsMut<BPlusTreeHeaderPage>();
  // empty
  bool is_empty = root_header_page->root_page_id_ == INVALID_PAGE_ID;
  if(is_empty) {
    WritePageGuard new_root = (bpm_->NewPageGuarded(&root_header_page->root_page_id_)).UpgradeWrite();
    LeafPage *root = new_root.AsMut<LeafPage>();
    root->Init();
    root->SetKeyAt(0, key);
    root->SetSize(1);
    root->SetValueAt(0, value);
    return true;
  }
  // find node
  std::queue<WritePageGuard> guards; // guards[i] the lock on node on the ith level
  std::queue<int> childs; // guards[i] is the childs[i]th child of guards[i - 1]
  childs.push(-1);
  guards.push(bpm_->FetchPageWrite(root_header_page->root_page_id_));
  const BPlusTreePage *root = guards.front().As<BPlusTreePage>();
  bool release_root = root->GetSize() == root->GetMaxSize();
  if(release_root)
    header_guard.Drop();
  InternalPage *tmp = guards.back().AsMut<InternalPage>();
  while(!tmp->IsLeafPage()) {
    int new_tmp = BinaryFind(tmp, key);
    guards.push(bpm_->FetchPageWrite(new_tmp));
    childs.push(new_tmp);
    tmp = guards.back().AsMut<InternalPage>();
    if(!tmp->IsLeafPage() && tmp->GetMaxSize() > tmp->GetSize()) {
      if(!release_root) {// header haven't been released 
        header_guard.Drop();
        release_root = true;
      }
      while(guards.size() > 1) {
        guards.pop();
        childs.pop();
      }
    }
  }
  WritePageGuard& leaf_target = guards.back();
  LeafPage* leaf_page = leaf_target.AsMut<LeafPage>();
  int leaf_pos = BinaryFind(leaf_page, key);
  if(leaf_pos == -1 || comparator_(key, leaf_page->KeyAt(leaf_pos)) == 0)
    return false;
  // with no split
  if(leaf_page->GetSize() < leaf_page->GetMaxSize()) {
    if(!release_root) {
      header_guard.Drop();
      release_root = true;
    }
    while(guards.size() > 1) {
      guards.pop();
      childs.pop();
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
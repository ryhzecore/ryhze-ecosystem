#include "editor_content_browser.hpp"
#include "desktop_meshes.hpp"
#include "editor_style.hpp"
#include "content_project.hpp"
#include <QFormLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStringList>
#include <QTextOption>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <algorithm>

namespace race::desktop::ui {
QString surface_profile_label(surface_pipeline::Profile profile){
  switch(profile){
    case surface_pipeline::Profile::PpmSrgb:return "PPM color (sRGB)";
    case surface_pipeline::Profile::PpmLinear:return "PPM data (linear)";
    case surface_pipeline::Profile::PpmSrgbMips:return "PPM color + mipmaps";
    case surface_pipeline::Profile::BmpSrgb:return "BMP color (sRGB)";
    case surface_pipeline::Profile::BmpLinear:return "BMP data (linear)";
    case surface_pipeline::Profile::BmpSrgbMips:return "BMP color + mipmaps";
    case surface_pipeline::Profile::PpmScalarMips:return "PPM scalar data + mipmaps";
    case surface_pipeline::Profile::BmpScalarMips:return "BMP scalar data + mipmaps";
    case surface_pipeline::Profile::Material:return "Opaque diffuse";
    case surface_pipeline::Profile::MaterialMips:return "Opaque diffuse + mipmaps";
    case surface_pipeline::Profile::MaterialScalarMips:return "Diffuse reflectance (R) + mipmaps";
  }return {};
}
namespace {
// A widget factory also supplies compact controls to the toolbar's native
// extension menu; addWidget's single default widget cannot serve both places.
class BrowserAction final : public QWidgetAction {
  QString control_name_;
protected:
  QWidget* createWidget(QWidget* parent) override {
    auto* control=button(parent,this,false,true);
    control->setObjectName(control_name_);return control;
  }
public:
  BrowserAction(QObject* parent,const QString& name,const QString& label)
      :QWidgetAction(parent),control_name_(name) {
    setObjectName(name+"Action");setText(label);setToolTip(label);
    setProperty("accessibleLabel",label);
  }
};
void text(QLineEdit* field,const QString& value) {
  if(field->text()!=value){field->setText(value);field->setCursorPosition(0);}
  field->setToolTip(value);
}
QLineEdit* field(QFormLayout* form,const char* name,const char* label) {
  auto* input=new QLineEdit;input->setObjectName(QString::fromLatin1(name));
  input->setReadOnly(true);input->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);
  input->setMinimumWidth(0);input->setFixedHeight(ShapeTokens::field_height);
  input->setAccessibleName(QString::fromLatin1(label));input->setFocusPolicy(Qt::StrongFocus);
  form->addRow(QString::fromLatin1(label),input);return input;
}
QString id(assets::AssetId value){return QString::fromStdString(assets::id_text(value));}
QString digest(const assets::Digest& value){return QString::fromStdString(assets::hex(value));}
QString source(const content_project::Resolved& entry){return std::visit([](const auto& recipe){return QString::fromStdString(recipe.definition.source);},entry.recipe);}
QString kind_name(content_project::Kind kind){switch(kind){case content_project::Kind::Mesh:return "Mesh";case content_project::Kind::Texture:return "Texture";case content_project::Kind::Material:return "Material";}return {};}
QString profile_name(const content_project::Resolved& entry){
  if(const auto* mesh=std::get_if<mesh_recipe::Snapshot>(&entry.recipe))return mesh->definition.profile==mesh_pipeline::Profile::GeometryUv?"Geometry + UVs":"Geometry only";
  return surface_profile_label(std::get<surface_recipe::Snapshot>(entry.recipe).definition.profile);
}
}
struct ContentBrowser::State {
  std::shared_ptr<const content_project::Snapshot> snapshot;
  std::optional<MeshReference> selected;
  std::optional<ContentCapture> checkpoint;
  std::optional<MaterialReference> selected_material;
  std::vector<MaterialReference> materials;
  std::vector<MeshReference> textures;
  QString project_path,checkpoint_path;
  std::vector<MeshReference> resident;
  QListWidget* rows{};
  QLineEdit *project{},*index{},*asset{},*source{},*profile{},*recipe{},*recipe_hash{},*scene_asset{},*scene_revision{};
  QLineEdit *checkpoint_path_field{},*checkpoint_hash{},*checkpoint_revision{},*source_hash{};
  QPlainTextEdit* revisions{};
  QAction *new_project{},*open{},*add_recipe{},*refresh{},*cancel{},*reimport{},*update{};
  bool browsing{},scene_busy{},can_update{},can_reimport{},operation_cancellable{},can_create{},can_add{};
};
ContentBrowser::ContentBrowser(QWidget* parent):QWidget(parent),state_(std::make_unique<State>()) {
  setObjectName("ContentBrowser");setAccessibleName("Project content browser");
  setMinimumHeight(0);setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Expanding);
  auto* body=new QVBoxLayout(this);body->setContentsMargins(0,0,0,0);body->setSpacing(0);
  auto* toolbar=new QToolBar(this);toolbar->setObjectName("ContentToolbar");
  toolbar->setAccessibleName("Project content actions");toolbar->setMovable(false);toolbar->setFloatable(false);
  toolbar->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);toolbar->setMinimumWidth(0);
  const auto add_action=[&](const char* name,const char* label,std::function<void()>* callback){
    auto* action=new BrowserAction(toolbar,QString::fromLatin1(name),QString::fromLatin1(label));
    QObject::connect(action,&QAction::triggered,this,[callback]{if(*callback)(*callback)();});
    toolbar->addAction(action);return action;
  };
  state_->new_project=add_action("ContentNewProject","New project",&on_new_project);
  state_->open=add_action("ContentOpen","Open project",&on_open);
  state_->add_recipe=add_action("ContentAddRecipe","Add recipe",&on_add_recipe);
  state_->refresh=add_action("ContentRefresh","Refresh",&on_refresh);
  state_->reimport=add_action("ContentReimport","Import/reimport",&on_reimport);
  state_->cancel=add_action("ContentCancel","Cancel",&on_cancel);
  state_->update=add_action("ContentUpdate","Update selected",&on_update);body->addWidget(toolbar);
  auto* split=new QSplitter(Qt::Horizontal,this);split->setObjectName("ContentSplit");split->setChildrenCollapsible(false);
  state_->rows=new QListWidget(split);state_->rows->setObjectName("ContentRows");
  state_->rows->setAccessibleName("Project assets");state_->rows->setMinimumWidth(160);
  state_->rows->setSelectionMode(QAbstractItemView::SingleSelection);
  state_->rows->setEditTriggers(QAbstractItemView::NoEditTriggers);
  state_->rows->setTextElideMode(Qt::ElideRight);state_->rows->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  state_->rows->setFocusPolicy(Qt::StrongFocus);
  auto* scroll=new QScrollArea(split);scroll->setObjectName("ContentDetailsScroll");
  scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);scroll->setMinimumWidth(0);
  scroll->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Expanding);
  auto* details=new QWidget;details->setObjectName("ContentDetails");
  auto* form=new QFormLayout(details);form->setContentsMargins(10,6,10,8);form->setVerticalSpacing(4);
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);form->setRowWrapPolicy(QFormLayout::DontWrapRows);
  state_->asset=field(form,"ContentAsset","Asset ID");
  state_->source=field(form,"ContentSource","Source");
  state_->profile=field(form,"ContentProfile","Profile");
  state_->recipe=field(form,"ContentRecipe","Recipe");
  state_->recipe_hash=field(form,"ContentRecipeHash","Recipe SHA-256");
  state_->project=field(form,"ContentProject","Project path");
  state_->index=field(form,"ContentIndexHash","Index SHA-256");
  state_->checkpoint_path_field=field(form,"ContentCheckpoint","Checkpoint path");
  state_->checkpoint_hash=field(form,"ContentCheckpointHash","Checkpoint SHA-256");
  state_->checkpoint_revision=field(form,"ContentCheckpointRevision","Cooked revision");
  state_->source_hash=field(form,"ContentSourceHash","Source SHA-256");
  state_->checkpoint_path_field->setPlaceholderText("No matching captured checkpoint");
  state_->scene_asset=field(form,"ContentSceneAsset","Scene asset");
  state_->scene_revision=field(form,"ContentSceneRevision","Scene revision");
  state_->scene_asset->setPlaceholderText("No Scene mesh selected");state_->scene_revision->setPlaceholderText("No Scene mesh selected");
  state_->revisions=new QPlainTextEdit;state_->revisions->setObjectName("ContentResident");
  state_->revisions->setAccessibleName("Resident revisions for browser asset");
  state_->revisions->setReadOnly(true);state_->revisions->setTabChangesFocus(true);
  state_->revisions->setWordWrapMode(QTextOption::WrapAnywhere);state_->revisions->setMinimumWidth(0);
  state_->revisions->setMinimumHeight(72);state_->revisions->setMaximumHeight(110);
  state_->revisions->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
  form->addRow("Resident revisions",state_->revisions);
  scroll->setWidget(details);split->addWidget(state_->rows);split->addWidget(scroll);
  split->setStretchFactor(0,0);split->setStretchFactor(1,1);split->setSizes({180,400});body->addWidget(split,1);
  setFocusProxy(state_->rows);
  QObject::connect(state_->rows,&QListWidget::currentRowChanged,this,[this]{
    refresh_details();if(on_selection)on_selection();
  });
  refresh_details();
}
ContentBrowser::~ContentBrowser() {
  QObject::disconnect(state_->rows,nullptr,this,nullptr);
}
std::optional<assets::AssetId> ContentBrowser::selected_asset() const {
  const auto row=state_->rows->currentRow();
  if(!state_->snapshot||row<0||static_cast<std::size_t>(row)>=state_->snapshot->entries().size())return {};
  return state_->snapshot->entries()[static_cast<std::size_t>(row)].reference.id;
}
bool ContentBrowser::select_asset(assets::AssetId asset) {
  if(!state_->snapshot)return false;
  int row=0;
  for(const auto& entry:state_->snapshot->entries()){
    if(entry.reference.id==asset){
      state_->rows->setCurrentRow(row);
      if(auto* item=state_->rows->currentItem())state_->rows->scrollToItem(item);
      return true;
    }
    ++row;
  }
  return false;
}
void ContentBrowser::set_snapshot(std::shared_ptr<const content_project::Snapshot> snapshot,QString full_project_path) {
  const auto previous=selected_asset();
  if(!snapshot||!state_->snapshot||snapshot->revision()!=state_->snapshot->revision()||full_project_path!=state_->project_path){
    state_->checkpoint.reset();state_->checkpoint_path.clear();
  }
  state_->project_path=snapshot?full_project_path:QString{};
  {
    const QSignalBlocker blocked(state_->rows);state_->snapshot=std::move(snapshot);state_->rows->clear();
    text(state_->project,state_->snapshot?full_project_path:QString{});
    text(state_->index,state_->snapshot?digest(state_->snapshot->revision()):QString{});
    int selected=-1;
    if(state_->snapshot){int n=0;for(const auto& entry:state_->snapshot->entries()){
      auto* item=new QListWidgetItem((kind_name(entry.reference.kind)+"  ")+source(entry),state_->rows);
      item->setToolTip(id(entry.reference.id)+"\n"+QString::fromStdString(entry.reference.recipe));
      item->setData(Qt::AccessibleTextRole,source(entry)+"; asset "+id(entry.reference.id));
      if(previous&&*previous==entry.reference.id)selected=n;++n;
    }}
    if(selected<0&&state_->rows->count()>0)selected=0;state_->rows->setCurrentRow(selected);
  }
  refresh_details();if(on_selection)on_selection();
}
void ContentBrowser::set_scene(std::optional<MeshReference> selected,std::span<const ResolvedMesh> resident,
    std::optional<MaterialReference> material,std::span<const ResolvedMaterial> materials,std::span<const ResolvedTexture> textures) {
  const auto equal=[](const auto entries,const auto& references){return entries.size()==references.size()&&std::equal(entries.begin(),entries.end(),references.begin(),[](const auto& entry,const auto& reference){return entry.reference==reference;});};
  if(state_->selected==selected&&state_->selected_material==material&&equal(resident,state_->resident)&&equal(materials,state_->materials)&&equal(textures,state_->textures))return;
  std::vector<MeshReference> mesh_refs,texture_refs;std::vector<MaterialReference> material_refs;
  mesh_refs.reserve(resident.size());material_refs.reserve(materials.size());texture_refs.reserve(textures.size());
  for(const auto& entry:resident)mesh_refs.push_back(entry.reference);
  for(const auto& entry:materials)material_refs.push_back(entry.reference);
  for(const auto& entry:textures)texture_refs.push_back(entry.reference);
  state_->selected=selected;state_->selected_material=material;state_->resident=std::move(mesh_refs);state_->materials=std::move(material_refs);state_->textures=std::move(texture_refs);refresh_details();
}
void ContentBrowser::set_checkpoint(std::optional<mesh_checkpoint::Published> checkpoint,QString full_checkpoint_path) {
  if(!checkpoint){set_capture(std::nullopt);return;}
  const auto& d=checkpoint->definition;
  set_capture(ContentCapture{d.id,d.index_revision,d.recipe_revision,d.source,d.cooked_revision,checkpoint->revision},std::move(full_checkpoint_path));
}
void ContentBrowser::set_capture(std::optional<ContentCapture> checkpoint,QString full_checkpoint_path) {
  state_->checkpoint=std::move(checkpoint);state_->checkpoint_path=state_->checkpoint?std::move(full_checkpoint_path):QString{};
  refresh_details();
}
void ContentBrowser::set_busy(bool browsing,bool scene_busy,bool can_update,bool can_reimport,bool operation_cancellable,bool can_create,bool can_add) {
  state_->browsing=browsing;state_->scene_busy=scene_busy;state_->can_update=can_update;
  state_->can_reimport=can_reimport;state_->operation_cancellable=operation_cancellable;
  state_->can_create=can_create;state_->can_add=can_add;
  state_->new_project->setEnabled(can_create&&!browsing&&!scene_busy);
  state_->open->setEnabled(!browsing&&!scene_busy);
  state_->add_recipe->setEnabled(can_add&&bool(state_->snapshot)&&!browsing&&!scene_busy);
  state_->refresh->setEnabled(bool(state_->snapshot)&&!browsing&&!scene_busy);
  state_->cancel->setEnabled(browsing||operation_cancellable);
  const auto asset=selected_asset();
  state_->reimport->setEnabled(can_reimport&&!browsing&&!scene_busy&&asset.has_value());
  const auto entry=asset&&state_->snapshot?state_->snapshot->find(*asset):Result<const content_project::Resolved*>{fail(ErrorCode::NotFound,"No selection")};
  state_->update->setText(entry&&entry.value()->reference.kind==content_project::Kind::Material?"Assign material":"Update selected");
  state_->update->setEnabled(can_update&&!browsing&&!scene_busy&&asset.has_value());
}
void ContentBrowser::refresh_details() {
  const auto chosen=selected_asset();const content_project::Resolved* entry=nullptr;
  if(chosen){const auto found=state_->snapshot->find(*chosen);if(found)entry=found.value();}
  text(state_->asset,entry?id(entry->reference.id):QString{});
  text(state_->source,entry?source(*entry):QString{});
  text(state_->profile,entry?profile_name(*entry):QString{});
  text(state_->recipe,entry?QString::fromStdString(entry->reference.recipe):QString{});
  text(state_->recipe_hash,entry?digest(entry->reference.revision):QString{});
  const auto* checkpoint=entry&&state_->checkpoint&&state_->checkpoint->id==entry->reference.id&&
      state_->checkpoint->index_revision==state_->snapshot->revision()&&
      state_->checkpoint->recipe_revision==entry->reference.revision?&*state_->checkpoint:nullptr;
  text(state_->checkpoint_path_field,checkpoint?state_->checkpoint_path:QString{});
  text(state_->checkpoint_hash,checkpoint?digest(checkpoint->revision):QString{});
  text(state_->checkpoint_revision,checkpoint?digest(checkpoint->cooked_revision):QString{});
  text(state_->source_hash,checkpoint?digest(checkpoint->source):QString{});
  const bool material=entry&&entry->reference.kind==content_project::Kind::Material;
  const auto scene=entry&&entry->reference.kind==content_project::Kind::Texture?std::optional<MeshReference>{}:material?state_->selected_material:state_->selected;
  text(state_->scene_asset,scene?id(scene->asset):QString{});
  text(state_->scene_revision,scene?digest(scene->revision):QString{});
  QStringList revisions;
  const auto& resident=material?state_->materials:entry&&entry->reference.kind==content_project::Kind::Texture?state_->textures:state_->resident;
  if(chosen)for(const auto& reference:resident)if(reference.asset==*chosen)revisions.push_back(digest(reference.revision));
  revisions.removeDuplicates();revisions.sort();const auto value=revisions.join('\n');
  if(state_->revisions->toPlainText()!=value)state_->revisions->setPlainText(value);
  state_->revisions->setPlaceholderText(chosen?"No resident revisions for this asset":"Select a project asset");
  state_->revisions->setToolTip(value);
  set_busy(state_->browsing,state_->scene_busy,state_->can_update,state_->can_reimport,state_->operation_cancellable,state_->can_create,state_->can_add);
}
} // namespace race::desktop::ui

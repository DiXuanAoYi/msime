import '../styles/modules/translation-service-links.css';
export type ServiceLink={label:string;url:string};
export function translationServiceLinks(provider:string):ServiceLink[] {
  if(provider==='tencent')return [
    {label:'腾讯云翻译 · 服务与开通',url:'https://cloud.tencent.com/product/tmt'},
    {label:'凭据控制台',url:'https://console.cloud.tencent.com/cam/capi'},
    {label:'接入与迁移说明',url:'https://cloud.tencent.com/document/product/551/104415'}
  ];
  if(provider==='niutrans')return [
    {label:'小牛翻译 · 注册与控制台',url:'https://niutrans.com'},
    {label:'翻译 API 文档',url:'https://niutrans.com/documents/contents/trans_text'}
  ];
  if(provider==='custom')return [{label:'DeepLX 项目与部署说明',url:'https://github.com/OwO-Network/DeepLX'}];
  return [];
}
export function renderTranslationServiceLinks(id:string,provider:string):void {
  const root=document.getElementById(id);if(!root)return;
  root.replaceChildren();
  for(const link of translationServiceLinks(provider)){
    const a=document.createElement('a');a.href=link.url;a.target='_blank';a.rel='noopener noreferrer';
    a.textContent=`${link.label}：${link.url}`;root.append(a);
  }
  root.hidden=!root.childElementCount;
}

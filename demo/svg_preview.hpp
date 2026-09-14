#pragma once
#include "deepnestcpp/geometry.hpp"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <algorithm>
#include <unordered_map>

namespace deepnest::cli {
inline std::string xmlText(const std::string& text) {
  std::string out;
  for(unsigned char c:text) {
    if(c=='&') out+="&amp;"; else if(c=='<') out+="&lt;"; else if(c=='>') out+="&gt;";
    else if(c=='\"') out+="&quot;"; else if(c=='\'') out+="&apos;";
    else if(c>=32 || c=='\n' || c=='\t') out+=char(c);
  }
  return out;
}
inline void exportSvg(const std::filesystem::path& path,const std::vector<Polygon>& sheets,
                      const std::vector<Polygon>& parts,const PlacementResult& result) {
  std::ofstream out(path,std::ios::binary);
  if(!out) throw std::runtime_error("Cannot create SVG preview");
  out.imbue(std::locale::classic()); out<<std::setprecision(12);
  std::vector<const Polygon*> shown;
  for(const auto& s:result.placements) {
    auto it=std::find_if(sheets.begin(),sheets.end(),[&](const Polygon& p) {return p.id==s.sheetid;});
    if(it!=sheets.end()) shown.push_back(&*it);
  }
  if(shown.empty() && !sheets.empty()) shown.push_back(&sheets[0]);
  double width=20,height=0;
  for(const auto* s:shown) { const auto b=getPolygonBounds(s->points); width+=b.width+20; height=std::max(height,b.height); }
  const double font=std::max(3.0,std::min(20.0,height/25));
  out<<"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1200\" viewBox=\"0 0 "<<width<<' '<<height+font*5+20<<"\">\n"
     <<"<rect width=\"100%\" height=\"100%\" fill=\"#e5e7eb\"/>\n"
     <<"<text x=\"10\" y=\""<<font*1.5<<"\" font-family=\"sans-serif\" font-size=\""<<font
     <<"\">Placed: "<<parts.size()-result.unplaced.size()<<" / "<<parts.size()<<" | Unplaced: "<<result.unplaced.size()<<"</text>\n";
  std::unordered_map<int,const Polygon*> originals;
  for(const auto& p:parts) if(p.id) originals[*p.id]=&p;
  auto contour=[&](const Polygon& p) {
    if(p.points.empty()) return;
    out<<'M'<<p.points[0].x<<','<<p.points[0].y;
    for(size_t i=1;i<p.points.size();++i) out<<'L'<<p.points[i].x<<','<<p.points[i].y;
    out<<'Z';
  };
  auto material=[&](const Polygon& p,const char* color,const std::string& label) {
    out<<"<path fill=\""<<color<<"\" stroke=\"#334155\" stroke-width=\"0.4\" fill-rule=\"evenodd\" d=\"";
    contour(p); for(const auto& h:p.children) contour(h);
    out<<"\"><title>"<<xmlText(label)<<"</title></path>\n";
  };
  const char* colors[]={"#38bdf8","#fb923c","#4ade80","#c084fc","#f472b6","#facc15"};
  double x=10;
  for(const auto* sheet:shown) {
    const auto b=getPolygonBounds(sheet->points);
    out<<"<text x=\""<<x<<"\" y=\""<<font*3<<"\" font-family=\"sans-serif\" font-size=\""<<font
       <<"\">"<<xmlText(sheet->source)<<" #"<<sheet->id.value_or(0)<<"</text>\n"
       <<"<g transform=\"translate("<<x-b.x<<','<<b.y+b.height+font*4<<") scale(1,-1)\">\n";
    material(*sheet,"#ffffff",sheet->source);
    for(const auto& s:result.placements) if(s.sheetid==sheet->id) for(const auto& p:s.sheetplacements) {
      auto it=originals.find(p.id.value_or(0));
      if(it==originals.end()) throw std::runtime_error("Unknown part in SVG preview");
      auto shape=shiftPolygon(rotatePolygon(*it->second,p.rotation),{p.x,p.y,true});
      material(shape,colors[size_t(p.id.value_or(0))%6],p.source+" #"+std::to_string(p.id.value_or(0))+" | angle "+std::to_string(p.rotation));
    }
    out<<"</g>\n"; x+=b.width+20;
  }
  out<<"</svg>\n"; out.flush();
  if(!out) throw std::runtime_error("Failed to write SVG preview");
}
}

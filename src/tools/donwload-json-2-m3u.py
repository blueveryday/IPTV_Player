import requests
import json
import csv
from urllib.parse import urlparse

def clean_play_url(url):
    """清理 URL 并仅保留 rtsp 协议"""
    parsed = urlparse(url.strip())
    if parsed.scheme != "rtsp":
        return None
    return f"{parsed.scheme}://{parsed.netloc}{parsed.path}"

def download_and_generate_m3u():
    url = "http://123.147.117.163:8081/service/100000000000001.json"
    try:
        response = requests.get(url, timeout=10)
        response.raise_for_status()

        with open("100000000000001.json", "wb") as json_file:
            json_file.write(response.content)
        print("原始JSON文件已保存：100000000000001.json")
        
    except requests.exceptions.RequestException as e:
        print(f"下载失败: {e}")
        return

    try:
        channels = response.json()
    except json.JSONDecodeError:
        print("JSON 解析失败")
        return

    m3u_content = ["#EXTM3U"]
    seen = set()
    epg_rows = []
    epg_seen = set()

    for channel in channels:
        name = channel.get("name", "").strip()
        play_url = channel.get("playUrl", "").strip()
        channel_number = str(channel.get("channelNumber", "")).strip()
        content_id = str(channel.get("contentID", "")).strip()

        if name and (channel_number or content_id):
            epg_key = (channel_number, content_id, name)
            if epg_key not in epg_seen:
                epg_seen.add(epg_key)
                epg_rows.append({
                    "channelnum": channel_number,
                    "channelcode": content_id,
                    "title": name,
                })

        if not name or not play_url:
            continue

        for url in play_url.split(";"):
            clean_url = clean_play_url(url)
            if not clean_url:
                continue

            key = (name, clean_url)
            if key not in seen:
                seen.add(key)
                m3u_content.append(f"#EXTINF:-1,{name}")
                m3u_content.append(clean_url)

    output_filename = "ChannelList.m3u"
    with open(output_filename, "w", encoding="utf-8") as f:
        f.write("\n".join(m3u_content))
    print(f"M3U 文件已生成: {output_filename}")

    csv_filename = "channel_epg_chongqing.csv"
    with open(csv_filename, "w", encoding="utf-8-sig", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=["channelnum", "channelcode", "title"])
        writer.writeheader()
        writer.writerows(epg_rows)
    print(f"CSV 文件已生成: {csv_filename}（共 {len(epg_rows)} 条）")

if __name__ == "__main__":
    download_and_generate_m3u()
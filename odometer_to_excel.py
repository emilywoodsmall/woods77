# odometer_to_excel.py
# reads the odometer from the OdoLogger over bluetooth and saves it in an excel file
#
# install stuff first:
#   pip install pyserial openpyxl
#
# how to run:
#   1. pair your computer with "OdoLogger_TRUCK_01" in bluetooth settings
#   2. find the port name:
#        mac:     /dev/cu.OdoLogger_TRUCK_01   (type  ls /dev/cu.*  in terminal)
#        windows: COM5 or something (look in Device Manager > Ports, use the "Outgoing" one)
#   3. put the port below and run:  python odometer_to_excel.py

import serial
import openpyxl
import os
import time
import datetime
import random

# ===== SETTINGS =====
PORT = "/dev/cu.OdoLogger_TRUCK_01"   # change this!!
EXCEL_FILE = "fleet_odometer_log.xlsx"
TEST_MODE = False   # True = make up fake numbers (no car needed)
# ====================

KM_TO_MILES = 0.621371


def make_new_excel():
    wb = openpyxl.Workbook()
    ws = wb.active
    ws.title = "Log"
    ws.append(["Date", "Time", "Vehicle", "Odometer (km)", "Odometer (miles)", "Miles since last", "Source", "Battery (V)"])
    # make the top row bold
    for cell in ws[1]:
        cell.font = openpyxl.styles.Font(bold=True)
    ws.column_dimensions["A"].width = 12
    ws.column_dimensions["B"].width = 10
    ws.column_dimensions["C"].width = 14
    ws.column_dimensions["D"].width = 15
    ws.column_dimensions["E"].width = 17
    ws.column_dimensions["F"].width = 17
    ws.column_dimensions["G"].width = 11
    ws.column_dimensions["H"].width = 12

    ws2 = wb.create_sheet("Totals")
    ws2.append(["Vehicle", "First reading (mi)", "Last reading (mi)", "Total miles", "First date", "Last date"])
    for cell in ws2[1]:
        cell.font = openpyxl.styles.Font(bold=True)
    for col in ["A", "B", "C", "D", "E", "F"]:
        ws2.column_dimensions[col].width = 18

    wb.save(EXCEL_FILE)
    print("made new excel file: " + EXCEL_FILE)


def find_last_miles(ws, vehicle):
    # go through the rows backwards and find the last reading for this vehicle
    row = ws.max_row
    while row > 1:
        if ws.cell(row=row, column=3).value == vehicle:
            return ws.cell(row=row, column=5).value
        row = row - 1
    return None


def update_totals(wb):
    ws = wb["Log"]
    ws2 = wb["Totals"]

    # clear the old totals (keep the header)
    if ws2.max_row > 1:
        ws2.delete_rows(2, ws2.max_row)

    # figure out first and last reading for each vehicle
    first = {}
    last = {}
    first_date = {}
    last_date = {}
    for row in range(2, ws.max_row + 1):
        vehicle = ws.cell(row=row, column=3).value
        miles = ws.cell(row=row, column=5).value
        date = ws.cell(row=row, column=1).value
        if vehicle is None:
            continue
        if vehicle not in first:
            first[vehicle] = miles
            first_date[vehicle] = date
        last[vehicle] = miles
        last_date[vehicle] = date

    for vehicle in first:
        total = round(last[vehicle] - first[vehicle], 1)
        ws2.append([vehicle, first[vehicle], last[vehicle], total, first_date[vehicle], last_date[vehicle]])


def save_reading(vehicle, km, source, volts):
    if not os.path.exists(EXCEL_FILE):
        make_new_excel()

    try:
        wb = openpyxl.load_workbook(EXCEL_FILE)
    except PermissionError:
        print("CANT OPEN EXCEL FILE!! close it in excel and try again")
        return

    ws = wb["Log"]

    miles = round(km * KM_TO_MILES, 1)

    last_miles = find_last_miles(ws, vehicle)
    if last_miles is None:
        since_last = 0
    else:
        since_last = round(miles - last_miles, 1)

    # dont fill up the sheet if the car didnt move
    if last_miles is not None and since_last == 0:
        print(vehicle + " didnt move, not saving")
        return

    now = datetime.datetime.now()
    date = now.strftime("%Y-%m-%d")
    t = now.strftime("%H:%M:%S")

    ws.append([date, t, vehicle, km, miles, since_last, source, volts])
    update_totals(wb)

    try:
        wb.save(EXCEL_FILE)
        print("saved: " + date + " " + t + "  " + vehicle + "  " + str(miles) + " mi  (+" + str(since_last) + ")")
    except PermissionError:
        print("CANT SAVE!! close the excel file first. this reading was lost :(")


def handle_line(line):
    # lines look like:  ODO,TRUCK_01,12345.6,J1939-HR,13.8
    # (old version of the logger only sends ODO,TRUCK_01,12345.6)
    parts = line.split(",")
    if parts[0] == "ODO" and len(parts) >= 3:
        vehicle = parts[1]
        try:
            km = float(parts[2])
        except ValueError:
            print("bad number: " + line)
            return
        source = ""
        volts = None
        if len(parts) >= 4:
            source = parts[3]
        if len(parts) >= 5:
            try:
                volts = float(parts[4])
            except ValueError:
                volts = None
        save_reading(vehicle, km, source, volts)
    elif parts[0] == "ERR":
        print("logger says there is a problem: " + line)
        print("(key might be off, or useJ1939 is set wrong in the logger code)")
    else:
        print("got something weird: " + line)


# ---------- main program ----------

print("OdoLogger excel saver")

if TEST_MODE == True:
    print("TEST MODE - making fake readings")
    fake_km = 50000.0
    while True:
        fake_km = fake_km + random.randint(1, 30)
        handle_line("ODO,TEST_TRUCK," + str(round(fake_km, 1)) + ",J1939-HR,13.8")
        time.sleep(3)

while True:
    try:
        print("connecting to " + PORT + " ...")
        bt = serial.Serial(PORT, 9600, timeout=5)
        print("connected!")
        bt.write(b"READ\n")   # ask for a reading right away

        while True:
            line = bt.readline().decode("utf-8", errors="ignore").strip()
            if line != "":
                handle_line(line)

    except serial.SerialException as e:
        print("lost connection or couldnt connect: " + str(e))
        print("trying again in 10 seconds...")
        time.sleep(10)
    except KeyboardInterrupt:
        print("bye")
        break
